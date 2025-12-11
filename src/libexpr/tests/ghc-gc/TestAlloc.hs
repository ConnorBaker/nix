{-# LANGUAGE ForeignFunctionInterface #-}

-- | Minimal test module for GHC RTS integration.
module TestAlloc where

import Foreign.Ptr (Ptr, nullPtr, castPtr)
import Foreign.C.Types (CSize(..), CInt(..))
import Foreign.Marshal.Alloc (mallocBytes, free)
import Foreign.StablePtr (StablePtr, newStablePtr, deRefStablePtr, freeStablePtr, castStablePtrToPtr, castPtrToStablePtr)
import Control.Exception (catch, SomeException)
import System.Mem (performMajorGC)

-- | Simple allocation for testing.
foreign export ccall "test_alloc_bytes"
  testAllocBytes :: CSize -> IO (Ptr ())

testAllocBytes :: CSize -> IO (Ptr ())
testAllocBytes (CSize 0) = return nullPtr
testAllocBytes (CSize size) =
  (castPtr <$> mallocBytes (fromIntegral size))
    `catch` \(_ :: SomeException) -> return nullPtr

-- | Free memory.
foreign export ccall "test_free_bytes"
  testFreeBytes :: Ptr () -> IO ()

testFreeBytes :: Ptr () -> IO ()
testFreeBytes ptr | ptr == nullPtr = return ()
                  | otherwise = free ptr

-- | StablePtr wrapper for testing.
newtype PtrWrapper = PtrWrapper (Ptr ())

foreign export ccall "test_new_stable_ptr"
  testNewStablePtr :: Ptr () -> IO (Ptr ())

testNewStablePtr :: Ptr () -> IO (Ptr ())
testNewStablePtr ptr = do
  stable <- newStablePtr (PtrWrapper ptr)
  return (castStablePtrToPtr stable)

foreign export ccall "test_deref_stable_ptr"
  testDerefStablePtr :: Ptr () -> IO (Ptr ())

testDerefStablePtr :: Ptr () -> IO (Ptr ())
testDerefStablePtr stablePtr
  | stablePtr == nullPtr = return nullPtr
  | otherwise = do
      let stable = castPtrToStablePtr stablePtr :: StablePtr PtrWrapper
      PtrWrapper ptr <- deRefStablePtr stable
      return ptr

foreign export ccall "test_free_stable_ptr"
  testFreeStablePtr :: Ptr () -> IO ()

testFreeStablePtr :: Ptr () -> IO ()
testFreeStablePtr stablePtr
  | stablePtr == nullPtr = return ()
  | otherwise = do
      let stable = castPtrToStablePtr stablePtr :: StablePtr PtrWrapper
      freeStablePtr stable

-- | Trigger GC.
foreign export ccall "test_perform_gc"
  testPerformGC :: IO ()

testPerformGC :: IO ()
testPerformGC = performMajorGC

-- | Return a test value to verify FFI is working.
foreign export ccall "test_get_magic"
  testGetMagic :: IO CInt

testGetMagic :: IO CInt
testGetMagic = return 42
