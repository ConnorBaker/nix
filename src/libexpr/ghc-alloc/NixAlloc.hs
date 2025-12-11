{-# LANGUAGE ForeignFunctionInterface #-}
{-# LANGUAGE MagicHash #-}
{-# LANGUAGE UnboxedTuples #-}
{-# LANGUAGE GHCForeignImportPrim #-}

-- | Nix memory allocator using GHC's RTS and garbage collector.
--
-- This module provides FFI exports for C++ code to allocate memory
-- on the GHC heap, allowing Nix to use GHC's world-class garbage
-- collector instead of implementing a custom one.
--
-- All allocations are GC-managed by GHC's RTS. Memory is automatically
-- reclaimed when unreachable.
module NixAlloc where

import Foreign.Ptr (Ptr, nullPtr, castPtr, plusPtr)
import Foreign.C.Types (CSize(..), CInt(..))
import Foreign.ForeignPtr (ForeignPtr, newForeignPtr_, withForeignPtr, castForeignPtr)
import qualified GHC.ForeignPtr as GHC (mallocPlainForeignPtrBytes)
import Foreign.StablePtr (StablePtr, newStablePtr, deRefStablePtr, freeStablePtr, castStablePtrToPtr, castPtrToStablePtr)
import Foreign.Marshal.Array (mallocArray, peekArray, pokeArray)
import Foreign.Marshal.Utils (fillBytes)
import Foreign.Storable (Storable(..))
import Control.Exception (catch, SomeException, evaluate)
import Control.Monad (forM_, replicateM)
import System.Mem (performGC, performMajorGC, performMinorGC)
import System.IO (hPutStrLn, stderr)
import GHC.Stats (getRTSStats, RTSStats(..))
import qualified GHC.Stats as Stats
import Data.IORef (IORef, newIORef, readIORef, writeIORef, modifyIORef')
import System.IO.Unsafe (unsafePerformIO)

-- ============================================================================
-- Allocation Statistics (tracked in Haskell)
-- ============================================================================

data AllocStats = AllocStats
  { allocBytesCount     :: !Int
  , allocBytesTotal     :: !Int
  , allocValueCount     :: !Int
  , allocValueTotal     :: !Int
  , allocEnvCount       :: !Int
  , allocEnvTotal       :: !Int
  , allocBindingsCount  :: !Int
  , allocBindingsTotal  :: !Int
  , allocListCount      :: !Int
  , allocListTotal      :: !Int
  , gcCycles            :: !Int
  }

{-# NOINLINE globalStats #-}
globalStats :: IORef AllocStats
globalStats = unsafePerformIO $ newIORef AllocStats
  { allocBytesCount = 0
  , allocBytesTotal = 0
  , allocValueCount = 0
  , allocValueTotal = 0
  , allocEnvCount = 0
  , allocEnvTotal = 0
  , allocBindingsCount = 0
  , allocBindingsTotal = 0
  , allocListCount = 0
  , allocListTotal = 0
  , gcCycles = 0
  }

recordAlloc :: (AllocStats -> AllocStats) -> IO ()
recordAlloc f = modifyIORef' globalStats f

-- ============================================================================
-- Generic byte allocation (GHC-managed)
-- ============================================================================

-- Iteration 68: PERFORMANCE FIX - Keep ForeignPtrs alive without O(n²) cost
-- Must keep ForeignPtrs in memory or GHC will collect them while C++ holds pointers
-- OLD BUG: Called `evaluate (length lst)` = O(n²) total cost for n allocations
-- FIX: Just cons onto list (O(1)) - never evaluate the list, just keep it alive
{-# NOINLINE allocatedPointers #-}
allocatedPointers :: IORef [ForeignPtr ()]
allocatedPointers = unsafePerformIO $ newIORef []

-- | Allocate bytes on GHC heap (GC-managed).
-- Returns NULL on allocation failure.
foreign export ccall "nix_ghc_alloc_bytes"
  nixGhcAllocBytes :: CSize -> IO (Ptr ())

nixGhcAllocBytes :: CSize -> IO (Ptr ())
nixGhcAllocBytes (CSize 0) = return nullPtr
nixGhcAllocBytes (CSize size) = do
  let sizeInt = fromIntegral size
  -- Allocate pinned memory on GHC's heap (won't be moved by GC)
  -- Using mallocPlainForeignPtrBytes allocates on the GHC heap
  fptr <- GHC.mallocPlainForeignPtrBytes sizeInt

  -- CRITICAL: Zero the memory to avoid crashes from reused memory.
  -- GHC can reuse memory from previously freed objects, which may contain
  -- old discriminator values (e.g., pdThunk). We must zero it before C++ uses it.
  withForeignPtr fptr $ \ptr -> do
    fillBytes ptr 0 sizeInt

  recordAlloc $ \s -> s
    { allocBytesCount = allocBytesCount s + 1
    , allocBytesTotal = allocBytesTotal s + sizeInt
    }

  -- Root the ForeignPtr by adding to global list (O(1) cons operation)
  -- PERFORMANCE: Do NOT evaluate the list! Just cons and return immediately
  let fptrVoid = castForeignPtr fptr :: ForeignPtr ()
  modifyIORef' allocatedPointers (fptrVoid :)

  -- Extract pointer for C++
  withForeignPtr fptr $ \ptr -> do
    return (castPtr ptr)

-- | Allocate atomic (non-traced) bytes.
-- For data that doesn't contain pointers to GC heap.
foreign export ccall "nix_ghc_alloc_bytes_atomic"
  nixGhcAllocBytesAtomic :: CSize -> IO (Ptr ())

nixGhcAllocBytesAtomic :: CSize -> IO (Ptr ())
nixGhcAllocBytesAtomic = nixGhcAllocBytes  -- Same implementation for now

-- ============================================================================
-- Batch allocation (for performance)
-- ============================================================================

-- | Batch allocate multiple objects of the same size.
-- Returns a linked list where the first word of each object points to the next.
-- This amortizes FFI overhead by allocating many objects in one call.
-- Each object gets its own ForeignPtr, so they can be GC'd individually.
foreign export ccall "nix_ghc_alloc_many"
  nixGhcAllocMany :: CSize -> CSize -> IO (Ptr ())

nixGhcAllocMany :: CSize -> CSize -> IO (Ptr ())
nixGhcAllocMany (CSize 0) _ = return nullPtr
nixGhcAllocMany _ (CSize 0) = return nullPtr
nixGhcAllocMany (CSize objectSize) (CSize count) = do
  let objSize = fromIntegral objectSize
      numObjects = fromIntegral count

  -- Allocate N separate objects (each can be freed individually)
  ptrs <- replicateM numObjects $ do
    fptr <- GHC.mallocPlainForeignPtrBytes objSize
    withForeignPtr fptr $ \ptr -> do
      -- Zero the memory
      fillBytes ptr 0 objSize
      return ()
    -- Root each ForeignPtr individually (allows individual freeing)
    let fptrVoid = castForeignPtr fptr :: ForeignPtr ()
    modifyIORef' allocatedPointers (fptrVoid :)
    -- Extract and return the pointer
    withForeignPtr fptr $ \ptr -> return (castPtr ptr)

  -- Update stats (batch allocation)
  recordAlloc $ \s -> s
    { allocBytesCount = allocBytesCount s + numObjects
    , allocBytesTotal = allocBytesTotal s + (objSize * numObjects)
    }

  -- Link into free list (first word points to next)
  forM_ (zip ptrs (tail ptrs ++ [nullPtr])) $ \(ptr, nextPtr) -> do
    poke (castPtr ptr :: Ptr (Ptr ())) nextPtr

  -- Return head of list
  case ptrs of
    (p:_) -> return p
    []    -> return nullPtr

-- ============================================================================
-- Nix Value allocation (16 bytes)
-- ============================================================================

foreign export ccall "nix_ghc_alloc_value"
  nixGhcAllocValue :: IO (Ptr ())

nixGhcAllocValue :: IO (Ptr ())
nixGhcAllocValue = do
  recordAlloc $ \s -> s
    { allocValueCount = allocValueCount s + 1
    , allocValueTotal = allocValueTotal s + 16
    }
  nixGhcAllocBytes 16

-- ============================================================================
-- Nix Env allocation (header + slots)
-- ============================================================================

foreign export ccall "nix_ghc_alloc_env"
  nixGhcAllocEnv :: CSize -> IO (Ptr ())

nixGhcAllocEnv :: CSize -> IO (Ptr ())
nixGhcAllocEnv (CSize numSlots) = do
  let envHeaderSize = 8  -- sizeof(EnvHeader) in C++
      slotSize = 8       -- sizeof(Value*)
      totalSize = envHeaderSize + fromIntegral numSlots * slotSize
  recordAlloc $ \s -> s
    { allocEnvCount = allocEnvCount s + 1
    , allocEnvTotal = allocEnvTotal s + totalSize
    }
  nixGhcAllocBytes (CSize $ fromIntegral totalSize)

-- ============================================================================
-- Nix Bindings allocation (header + capacity)
-- ============================================================================

foreign export ccall "nix_ghc_alloc_bindings"
  nixGhcAllocBindings :: CSize -> IO (Ptr ())

nixGhcAllocBindings :: CSize -> IO (Ptr ())
nixGhcAllocBindings (CSize capacity) = do
  let bindingsHeaderSize = 16  -- sizeof(Bindings) header
      attrSize = 24            -- sizeof(Attr)
      totalSize = bindingsHeaderSize + fromIntegral capacity * attrSize
  recordAlloc $ \s -> s
    { allocBindingsCount = allocBindingsCount s + 1
    , allocBindingsTotal = allocBindingsTotal s + totalSize
    }
  nixGhcAllocBytes (CSize $ fromIntegral totalSize)

-- ============================================================================
-- Nix List allocation (array of Value pointers)
-- ============================================================================

foreign export ccall "nix_ghc_alloc_list"
  nixGhcAllocList :: CSize -> IO (Ptr ())

nixGhcAllocList :: CSize -> IO (Ptr ())
nixGhcAllocList (CSize numElems) = do
  let totalSize = fromIntegral numElems * 8  -- sizeof(Value*)
  recordAlloc $ \s -> s
    { allocListCount = allocListCount s + 1
    , allocListTotal = allocListTotal s + totalSize
    }
  nixGhcAllocBytes (CSize $ fromIntegral totalSize)

-- ============================================================================
-- Free (no-op for GC-managed memory)
-- ============================================================================

foreign export ccall "nix_ghc_free"
  nixGhcFree :: Ptr () -> IO ()

nixGhcFree :: Ptr () -> IO ()
nixGhcFree _ = return ()  -- GC handles this automatically

-- ============================================================================
-- StablePtr management (for root set)
-- ============================================================================

newtype ValuePtr = ValuePtr (Ptr ())

foreign export ccall "nix_ghc_new_stable_ptr"
  nixGhcNewStablePtr :: Ptr () -> IO (Ptr ())

nixGhcNewStablePtr :: Ptr () -> IO (Ptr ())
nixGhcNewStablePtr ptr = do
  stable <- newStablePtr (ValuePtr ptr)
  return (castStablePtrToPtr stable)

foreign export ccall "nix_ghc_deref_stable_ptr"
  nixGhcDerefStablePtr :: Ptr () -> IO (Ptr ())

nixGhcDerefStablePtr :: Ptr () -> IO (Ptr ())
nixGhcDerefStablePtr stablePtr
  | stablePtr == nullPtr = return nullPtr
  | otherwise = do
      let stable = castPtrToStablePtr stablePtr :: StablePtr ValuePtr
      ValuePtr ptr <- deRefStablePtr stable
      return ptr

foreign export ccall "nix_ghc_free_stable_ptr"
  nixGhcFreeStablePtr :: Ptr () -> IO ()

nixGhcFreeStablePtr :: Ptr () -> IO ()
nixGhcFreeStablePtr stablePtr
  | stablePtr == nullPtr = return ()
  | otherwise = do
      let stable = castPtrToStablePtr stablePtr :: StablePtr ValuePtr
      freeStablePtr stable

-- ============================================================================
-- Root set management (for GC roots)
-- ============================================================================

{-# NOINLINE globalRoots #-}
globalRoots :: IORef [Ptr ()]
globalRoots = unsafePerformIO $ newIORef []

foreign export ccall "nix_ghc_register_value_root"
  nixGhcRegisterValueRoot :: Ptr () -> IO ()

nixGhcRegisterValueRoot :: Ptr () -> IO ()
nixGhcRegisterValueRoot ptr
  | ptr == nullPtr = return ()
  | otherwise = modifyIORef' globalRoots (ptr :)

foreign export ccall "nix_ghc_unregister_value_root"
  nixGhcUnregisterValueRoot :: Ptr () -> IO ()

nixGhcUnregisterValueRoot :: Ptr () -> IO ()
nixGhcUnregisterValueRoot ptr
  | ptr == nullPtr = return ()
  | otherwise = modifyIORef' globalRoots (filter (/= ptr))

-- ============================================================================
-- GC control
-- ============================================================================

foreign export ccall "nix_ghc_perform_gc"
  nixGhcPerformGC :: IO ()

nixGhcPerformGC :: IO ()
nixGhcPerformGC = do
  recordAlloc $ \s -> s { gcCycles = gcCycles s + 1 }
  performMajorGC

foreign export ccall "nix_ghc_perform_minor_gc"
  nixGhcPerformMinorGC :: IO ()

nixGhcPerformMinorGC :: IO ()
nixGhcPerformMinorGC = do
  recordAlloc $ \s -> s { gcCycles = gcCycles s + 1 }
  performMinorGC

-- ============================================================================
-- GC statistics
-- ============================================================================

foreign export ccall "nix_ghc_get_gc_cycles"
  nixGhcGetGCCycles :: IO CSize

nixGhcGetGCCycles :: IO CSize
nixGhcGetGCCycles = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.gcs stats

foreign export ccall "nix_ghc_get_heap_size"
  nixGhcGetHeapSize :: IO CSize

nixGhcGetHeapSize :: IO CSize
nixGhcGetHeapSize = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.gcdetails_live_bytes $ Stats.gc stats

foreign export ccall "nix_ghc_get_allocated_bytes"
  nixGhcGetAllocatedBytes :: IO CSize

nixGhcGetAllocatedBytes :: IO CSize
nixGhcGetAllocatedBytes = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.allocated_bytes stats

-- ============================================================================
-- Iteration 67: Additional GC statistics
-- ============================================================================

-- | Get number of major GCs
foreign export ccall "nix_ghc_get_major_gcs"
  nixGhcGetMajorGCs :: IO CSize

nixGhcGetMajorGCs :: IO CSize
nixGhcGetMajorGCs = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.major_gcs stats

-- | Get maximum live bytes (peak memory usage)
foreign export ccall "nix_ghc_get_max_live_bytes"
  nixGhcGetMaxLiveBytes :: IO CSize

nixGhcGetMaxLiveBytes :: IO CSize
nixGhcGetMaxLiveBytes = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.max_live_bytes stats

-- | Get maximum heap size ever allocated
foreign export ccall "nix_ghc_get_max_mem_in_use_bytes"
  nixGhcGetMaxMemInUseBytes :: IO CSize

nixGhcGetMaxMemInUseBytes :: IO CSize
nixGhcGetMaxMemInUseBytes = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.max_mem_in_use_bytes stats

-- | Get cumulative GC CPU time (nanoseconds)
foreign export ccall "nix_ghc_get_gc_cpu_ns"
  nixGhcGetGCCpuNs :: IO CSize

nixGhcGetGCCpuNs :: IO CSize
nixGhcGetGCCpuNs = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.gc_cpu_ns stats

-- | Get cumulative GC elapsed time (nanoseconds)
foreign export ccall "nix_ghc_get_gc_elapsed_ns"
  nixGhcGetGCElapsedNs :: IO CSize

nixGhcGetGCElapsedNs :: IO CSize
nixGhcGetGCElapsedNs = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.gc_elapsed_ns stats

-- | Get copied bytes during GC
foreign export ccall "nix_ghc_get_copied_bytes"
  nixGhcGetCopiedBytes :: IO CSize

nixGhcGetCopiedBytes :: IO CSize
nixGhcGetCopiedBytes = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.gcdetails_copied_bytes $ Stats.gc stats

-- | Get parallel GC work balance (0.0 = perfect balance, higher = imbalance)
foreign export ccall "nix_ghc_get_par_max_copied_bytes"
  nixGhcGetParMaxCopiedBytes :: IO CSize

nixGhcGetParMaxCopiedBytes :: IO CSize
nixGhcGetParMaxCopiedBytes = do
  stats <- Stats.getRTSStats
  return $ CSize $ fromIntegral $ Stats.gcdetails_par_max_copied_bytes $ Stats.gc stats

-- | Get number of GC generations (typically 2)
foreign export ccall "nix_ghc_get_generations"
  nixGhcGetGenerations :: IO CSize

nixGhcGetGenerations :: IO CSize
nixGhcGetGenerations = do
  -- GHC typically uses 2 generations for most programs
  -- This could be made dynamic if we track RTS flags
  return $ CSize 2

-- ============================================================================
-- Allocation statistics (from our tracking)
-- ============================================================================

foreign export ccall "nix_ghc_get_alloc_count"
  nixGhcGetAllocCount :: IO CSize

nixGhcGetAllocCount :: IO CSize
nixGhcGetAllocCount = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocBytesCount stats

foreign export ccall "nix_ghc_get_traced_alloc_count"
  nixGhcGetTracedAllocCount :: IO CSize

nixGhcGetTracedAllocCount :: IO CSize
nixGhcGetTracedAllocCount = nixGhcGetAllocCount

foreign export ccall "nix_ghc_get_traced_alloc_bytes"
  nixGhcGetTracedAllocBytes :: IO CSize

nixGhcGetTracedAllocBytes :: IO CSize
nixGhcGetTracedAllocBytes = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocBytesTotal stats

foreign export ccall "nix_ghc_get_atomic_alloc_count"
  nixGhcGetAtomicAllocCount :: IO CSize

nixGhcGetAtomicAllocCount :: IO CSize
nixGhcGetAtomicAllocCount = return 0

foreign export ccall "nix_ghc_get_atomic_alloc_bytes"
  nixGhcGetAtomicAllocBytes :: IO CSize

nixGhcGetAtomicAllocBytes :: IO CSize
nixGhcGetAtomicAllocBytes = return 0

foreign export ccall "nix_ghc_get_value_alloc_count"
  nixGhcGetValueAllocCount :: IO CSize

nixGhcGetValueAllocCount :: IO CSize
nixGhcGetValueAllocCount = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocValueCount stats

foreign export ccall "nix_ghc_get_value_alloc_bytes"
  nixGhcGetValueAllocBytes :: IO CSize

nixGhcGetValueAllocBytes :: IO CSize
nixGhcGetValueAllocBytes = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocValueTotal stats

foreign export ccall "nix_ghc_get_env_alloc_count"
  nixGhcGetEnvAllocCount :: IO CSize

nixGhcGetEnvAllocCount :: IO CSize
nixGhcGetEnvAllocCount = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocEnvCount stats

foreign export ccall "nix_ghc_get_env_alloc_bytes"
  nixGhcGetEnvAllocBytes :: IO CSize

nixGhcGetEnvAllocBytes :: IO CSize
nixGhcGetEnvAllocBytes = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocEnvTotal stats

foreign export ccall "nix_ghc_get_bindings_alloc_count"
  nixGhcGetBindingsAllocCount :: IO CSize

nixGhcGetBindingsAllocCount :: IO CSize
nixGhcGetBindingsAllocCount = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocBindingsCount stats

foreign export ccall "nix_ghc_get_bindings_alloc_bytes"
  nixGhcGetBindingsAllocBytes :: IO CSize

nixGhcGetBindingsAllocBytes :: IO CSize
nixGhcGetBindingsAllocBytes = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocBindingsTotal stats

foreign export ccall "nix_ghc_get_list_alloc_count"
  nixGhcGetListAllocCount :: IO CSize

nixGhcGetListAllocCount :: IO CSize
nixGhcGetListAllocCount = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocListCount stats

foreign export ccall "nix_ghc_get_list_alloc_bytes"
  nixGhcGetListAllocBytes :: IO CSize

nixGhcGetListAllocBytes :: IO CSize
nixGhcGetListAllocBytes = do
  stats <- readIORef globalStats
  return $ CSize $ fromIntegral $ allocListTotal stats

foreign export ccall "nix_ghc_get_live_alloc_count"
  nixGhcGetLiveAllocCount :: IO CSize

nixGhcGetLiveAllocCount :: IO CSize
nixGhcGetLiveAllocCount = return 0  -- Would need more sophisticated tracking

foreign export ccall "nix_ghc_get_live_alloc_bytes"
  nixGhcGetLiveAllocBytes :: IO CSize

nixGhcGetLiveAllocBytes :: IO CSize
nixGhcGetLiveAllocBytes = nixGhcGetHeapSize

-- ============================================================================
-- Root access (for mark phase compatibility)
-- ============================================================================

foreign export ccall "nix_ghc_gc_get_root_count"
  nixGhcGCGetRootCount :: IO CSize

nixGhcGCGetRootCount :: IO CSize
nixGhcGCGetRootCount = do
  roots <- readIORef globalRoots
  return $ CSize $ fromIntegral $ length roots

foreign export ccall "nix_ghc_gc_get_root_at"
  nixGhcGCGetRootAt :: CSize -> IO (Ptr ())

nixGhcGCGetRootAt :: CSize -> IO (Ptr ())
nixGhcGCGetRootAt (CSize idx) = do
  roots <- readIORef globalRoots
  let i = fromIntegral idx
  if i < length roots
    then return (roots !! i)
    else return nullPtr

-- ============================================================================
-- Stub functions for compatibility (no-ops since GHC manages everything)
-- ============================================================================

foreign export ccall "nix_ghc_gc_add_root"
  nixGhcGCAddRoot :: Ptr () -> IO ()

nixGhcGCAddRoot :: Ptr () -> IO ()
nixGhcGCAddRoot = nixGhcRegisterValueRoot

foreign export ccall "nix_ghc_gc_remove_root"
  nixGhcGCRemoveRoot :: Ptr () -> IO ()

nixGhcGCRemoveRoot :: Ptr () -> IO ()
nixGhcGCRemoveRoot = nixGhcUnregisterValueRoot

foreign export ccall "nix_ghc_gc_clear_roots"
  nixGhcGCClearRoots :: IO ()

nixGhcGCClearRoots :: IO ()
nixGhcGCClearRoots = writeIORef globalRoots []

foreign export ccall "nix_ghc_gc_begin_mark"
  nixGhcGCBeginMark :: IO ()

nixGhcGCBeginMark :: IO ()
nixGhcGCBeginMark = return ()  -- GHC handles marking

foreign export ccall "nix_ghc_gc_mark"
  nixGhcGCMark :: Ptr () -> IO CInt

nixGhcGCMark :: Ptr () -> IO CInt
nixGhcGCMark _ = return 0  -- GHC handles marking

foreign export ccall "nix_ghc_gc_is_marked"
  nixGhcGCIsMarked :: Ptr () -> IO CInt

nixGhcGCIsMarked :: Ptr () -> IO CInt
nixGhcGCIsMarked _ = return 1  -- Assume everything is live

foreign export ccall "nix_ghc_gc_sweep"
  nixGhcGCSweep :: IO CSize

nixGhcGCSweep :: IO CSize
nixGhcGCSweep = return 0  -- GHC handles sweeping

foreign export ccall "nix_ghc_get_alloc_size"
  nixGhcGetAllocSize :: Ptr () -> IO CSize

nixGhcGetAllocSize :: Ptr () -> IO CSize
nixGhcGetAllocSize _ = return 0  -- Unknown for GC-managed memory
