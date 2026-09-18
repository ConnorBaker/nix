# Defects pinned by a named test

The lazy-store work kept a ledger of every defect the branch introduced and found later, each with its root cause. What has lasting value from it is the table below: each defect the branch had, in one line, and the test that fails on the code that had it. Rows whose fix has no discriminating test are not listed; the tests still owed are `05-validation.md` section 7, and the facts the defects taught about the local store are `08-store-model.md`. The rule the table enforces is that a fix carries a test that failed first (`05-validation.md` section 4).

| defect | pinned by |
|---|---|
| `builtins.storePath` and `fetchTree` of a pending object dereferenced a name that was not written | `WriteBufferTest.everyBuiltinAgreesWithEagerEvaluationOnPendingInputs`, `dereferencingBuiltinsWriteFirst`, `tests/functional/lazy-store-doors.sh` |
| a pending path printed through `match`, `split`, `getContext` or `trace` before its object existed | the (C2) observer's `match`, `split`, `getctx`, `trace` modes; `WriteBufferTest.valuePrinterRealisesBeforeEmitting`, `ambiguousPrinterRealisesBeforeEmitting` |
| a context-free literal naming a pending path escaped, or could not be read | `WriteBufferTest.aContextFreeLiteralNamingAPendingPathCannotEscape`, `aContextFreeLiteralNamingAPendingPathCanBeRead` |
| the value printer's path text, the inline error text, the eval cache's getters, `realiseString` with an empty context, a path value's text left without a door | `WriteBufferTest.printedPathValueDoesNotEscape`, `printedInlineErrorDoesNotEscape`, `evalCacheGettersAreDoors`, `realiseStringWritesEverythingPending`; `nix_api_boundary_test.a_path_values_text_names_only_written_objects` |
| an error's text reached C, and an attribute's name, before the objects they named were written | `nix_api_boundary_test.error_text_names_only_written_objects`, `an_attribute_name_names_only_written_objects`, `a_returned_value_names_only_written_objects` |
| an exec-ending command ran no destructor and flushed nothing | `WriteBufferTest.finishWritesEverythingPending`; the compiler on the token |
| the pure JSON and XML serialisers flushed inside, or emitted before realising | `WriteBufferTest.jsonRenderedThenRealisedDoesNotEscape`, `xmlRenderedThenRealisedDoesNotEscape`, `serialisingIntoAValueWritesNothing` |
| the coercions flushed totally where a dereference should write iff pending | `WriteBufferTest.coercionsWriteWhatTheyDereferenceAndNothingForTheRest`, `discardingContextLeavesTheObjectPending`, `contextDroppingBuiltinsLeaveTheObjectsPending` |
| buffered repair was a silent no-op through the batch add | `WriteBufferLocalStoreTest.repairRewritesAnObjectTheDatabaseStillCallsValid` |
| pending objects' references were not temp-rooted between enqueue and flush | `WriteBufferLocalStoreTest.gcKeepsTheExistingReferencesOfPendingObjects` |
| a byte-size flush cap never fired | `WriteBufferTest.crossingTheThresholdFlushesToBoundMemory`, `autoFlushKeepsReferencesValidForLaterReferrers` |
| the derivation round trip through the buffer (and master's 0xFF and platform defects) | `WriteBufferTest.prop_derivations_round_trip_through_the_buffer`, `DerivationAtermTest.prop_unparse_parse_round_trip`, `aByteEqualToEofRoundTrips`, `platformOutsideTheFormatIsRefused` |
| the mounted accessor's missing-path error lost its class | `MountedSourceAccessor.missingPathRaisesTheResolvedTreesError` |
| a graft did not list at its parent; a mount over an existing entry listed twice | `MountedSourceAccessor.graftIsATree`, `mountOverExistingEntryIsListedOnce`, `mountingTwiceKeepsTheFirst`; `tests/functional/mounted-inputs-listed.sh` |
| the union was pointwise, not top-down | `UnionSourceAccessor.overlaySemantics`, `fingerprints` |
| the naming property omitted the filter and the coherent union | `SourceAccessorNaming.equalNamesImplyEqualTrees`, `coherentUnionNamesByTheFirstChildAndStaysSound`; `FetchToStore.prop_filtered_names_are_sound`, `prop_wrapped_dump_equals_filtered_dump` |
| the fixed-set filter answered in the inner accessor's coordinates | `FixedSetFilter.namesByTheInnerNameAndTheAdmittedSet`, `FetchToStore.prop_wrapped_dump_equals_filtered_dump` |
| the memo looked directories up under a filter | `FetchToStoreTest.gitSubtreeMemoRespectsFilters`, `filteredNamedTreesAreMemoised` |
| a bare executable or symlink root had no address, or two roots one path | `FetchToStoreTest.aBareRootIsAddressedByTheObjectHash`, `MerkleHash.objectHashCases`, `ObjectHashSink.prop_both_routes_match_reference_and_nar_length` (whose generator draws bare roots), `tests/functional/git-hashing/bare-root.sh` |
| a root's memo row held the object hash where the parent reads the entry | `GitUtilsTest.memoRowsHoldEntryHashesForBareExecutableAndSymlinkRoots`, `FetchToStoreTest.oldFormGitRowIsAMissAndTheRowIsTheEntry` |
| the export-ignore wrapper hid the names beneath it for every repository | `GitUtilsTest.exportIgnoreWrapperPassesNamesThroughWhenItHidesNothing`, `tests/functional/fetchGit-subtree-names.sh` |
| a dirty checkout was hashed whole; an untracked `.gitattributes` was not consulted | `GitUtilsTest.workdirNamesCleanSubtreesByTheCommit` |
| the tree sink refused every name libgit2's tree builder refuses | `GitUtilsTest.sinkWritesEveryNarName`, `GitTest.tree_encoding_round_trips_dot_git`, `nar_accepts_dot_git_and_rejects_dot_names` |
| the shim recorded a NAR↔tree pair from two walks of an unnamed tree; a filtered `sha256` wrapped twice | `GitUtilsTest.narHashOfIsTheMemoisedShim`, `AddPathTest.unnamedFilteredTreeWithSha256IsWalkedOnce`, `namedFilteredTreeWithSha256IsWalkedOnceAndPaired` |
| a version-7 lock's `narHash` was used as a name; a version-8 lock refused a node with `narHash` alone | `LockFile.version7IsReadWithItsNarHashAsAnAssertion`, `version8RoundTripsWithTreeHash`, `version8AcceptsANodeWithOnlyANarHash`, `version9IsRefused`; `tests/functional/git-hashing/source-addressing.sh`, `tests/functional/flakes/old-lockfiles.sh` |
| `treeHash` missing from a scheme's allowed attributes | `LockFile.version8RoundTripsWithTreeHash` under the mutation of `05-validation.md` section 4 |
| the NAR size unknown for a walk-registered path | `MerkleHash.narSizeEqualsDumpPathLength`, `narSizesOfFixedNodes` |
| the identifiers not git's | `ObjectHashSink.sha256_identifiers_are_gits`, `MerkleHash.blobHeaderAndIncrementalHasher`, `serialiseTreeIsGitsBody`, `GitUtilsTest.sha256IdentifiersAgreeWithLibgit2`; `git-hashing/simple-sha256.sh` |
| the hash over the filesystem's case-hacked names, not the tree's; the suffix accepted in a NAR | `ObjectHashSink.case_hack_suffix_is_not_hashed`, `LocalStoreObjectsTest.caseHackSuffixIsNotHashed`, `MerkleHash.unhackNameAndUnhackedEntries`, `NarTest.entryNameWithCaseHackSuffixIsRefusedUnderEitherSetting`, `hackedNameCollidingWithAnUpperCaseSuffixEntryIsRefused` |
| the accessor route and the NAR route disagreed; a `known` answer read beneath | `ObjectHashSink.prop_both_routes_match_reference_and_nar_length`, `complex_tree_matches_reference_and_nar_length`, `filter_matches_dumpPath`, `known_answers_without_reading`, `flat_createDirectory_form_is_refused` |
| the copy visitor lost the entry name; a derived sink saw the root's files only | `CopyRecursive.copies_a_tree_under_both_sink_conventions`; the two-level restores of `LocalStoreObjectsTest.restoreThroughObjects` |
| write-then-link on a hit; the streamed path untested | `LocalStoreObjectsTest.restoreThroughObjects`, `oneChangedFileWritesOneFile`, `restoreThroughObjectsStreamsLargeFiles`, `repairDoesNotLinkACorruptBlob` (the counting hooks, under the `link` mutation); `RestoreSink.beginRegularFile_then_finish_equals_createRegularFile` |
| a corrupt blob linked under repair | `LocalStoreObjectsTest.repairDoesNotLinkACorruptBlob`, `repairRelinksAFreshFileOverACorruptBlob`; `object-store.sh`'s corruption block, which corrupts a real file's blob |
| a synthetic root tree unwritten or read as malformed | `LocalStoreObjectsTest.syntheticRootTreeIsWrittenAndKeepsItsBlob` |
| the sweep kept only symlink-target blobs; abandoned on a missing live root; counted subtrees as roots; aborted on a missing directory; trusted a corrupt tree | `LocalStoreObjectsTest.sweepKeepsABlobALiveTreeNamesThoughNothingLinksIt`, `sweepKeepsAPlainFileRootsBlob`, `sweepAndVerifyOnALiveRootWhoseTreeIsMissing`, `sweepTolerantOfAnUnenteredRoot`, `sweepCountsRootsApartFromMissingSubtrees`, `sweepMakesAMissingObjectDirectory`, `sweepAbortsOnACorruptTree` |
| the ingestion's `autoGC()` swept its own trees | `LocalStoreAutoGCTest.addToStoresOwnCollectionKeepsItsObjects`, `spilledAddToStoreFromDumpsOwnCollectionKeepsItsObjects` |
| a stale temporary link pinned its blob for ever | `LocalStoreObjectsTest.staleTemporaryLinkGoesWithTheBlobItPinned` |
| the verifier expected a blob the store does not place | `LocalStoreObjectsTest.verifyExpectsNoBlobForAFileTheStoreDoesNotPlace` |
| `--optimise` entered paths and never wrote the column | `LocalStoreObjectsTest.optimiseStoreMigratesAnOldRowAndEntersNoModifiedPath`, `enterIntoObjectsAndMigrate`, `flatAddIsEnteredAndRegistersItsObjectHash`; `object-hash.sh` |
| a modified schema-10 path migrated with the corrupt tree's hash; a file-less row threw | `LocalStoreObjectsTest.modifiedSchema10RowIsNotMigrated`, `schema10RowWithMissingFilesIsAnsweredFromTheDatabase`, `ObjectHashLocalStore.verifyStore_checkContents_reports_a_modified_path` |
| the migration rooted dead paths during a collection | `LocalStoreObjectsTest.migrationDoesNotRootThePath`, `gcUnderKeepDerivationsDeletesASchema10Output` |
| an old row not migrated on read, or written by a read-only store | `LocalStoreObjectsTest.migrationDoesNotRootThePath` (the row migrated on the first query, current afterwards), `ObjectHashLocalStore.read_only_store_does_not_write_the_migration`, `migratePathInfo_migrates_an_old_row`, `schema_is_11_and_newer_is_refused` |
| the version-4 JSON rendered signatures as strings; formats 1–3 lacked the NAR hash | `ObjectHashPathInfoJsonV4.*` |
| a `.narinfo` with one hash field refused | `ObjectHashNarInfo.*` |
| the wire slot misread without the feature; the old form sent without a NAR hash | `ObjectHashWorkerProto.*`, `ObjectHashServeProto.*` |
| a description with neither hash and no content address registered | `LocalStoreObjectsTest.addToStoreRefusesAnInfoWithNoContentHash` |
| signatures version 2 only; a NarHash-only substitution untrusted | `ObjectHashPathInfo.signV1_verifies_under_fingerprintV1`, `checkSignatures_counts_a_key_once_across_both_fingerprints`, `checkSignatures_accepts_v1_only_with_assertedNarHash`; `LocalStoreObjectsTest.versionOneSignatureWithoutAssertedNarHashIsCheckedOnTheStream`, `versionOneSignatureOnARowVerifiesWithTheWalk`; `signing.sh`, `object-hash.sh` |
| a new client against an old daemon dereferenced an absent object hash | `object-hash.sh`, the reference-daemon block |
| old peers refusing git-named sources, silently enabled in the test | `object-hash.sh`, both directions without and with `git-hashing` |
| the SHA-1 form created, or refused by a reader | `FileIngestionMethod.gitMethodHashesUnderSha256Only`, `LocalStoreObjectsTest.gitMethodRefusesSha1AtEveryCreationEntry`, `sha1GitRowIsReadVerifiedAndCollected`, `ContentAddress.sha1GitFormIsReadable`, `DerivationAtermTest.sha1GitFixedOutputReads`; `git-hashing/simple-sha1.sh`, `fixed.sh`, `ca/floating-git.sh` |
| the pack deflated what would not shrink | `GitUtilsTest.packsStoreWhatWouldNotShrink` |
| a specific delete swept the object store; its freed bytes said nothing of the sweep | `git-hashing/object-store-fetch.sh`, `object-store.sh` |
| `.links` never reclaimed by the collector on an upgraded store | `object-store.sh` |
| the removed setting an error on the command line; warned twice per build; in the key=value dump | `optimise-store.sh`, `cli-characterisation.sh` |
| `--load-db` aborted the whole load on the first record whose NAR hash did not match, before anything was registered | `object-hash.sh`'s `--load-db` block: three old-form records, one asserting another path's hash; the two sound records registered, the bad one reported with both hashes, exit 1 |
| `max-free` met shared files | `gc-auto.sh` |
| a test read the developer's cache directory; relied on substitution in a sandbox | `AddPathTest.namedFilteredTreeWithSha256IsWalkedOnceAndPaired` (the fixture's `NIX_CACHE_HOME`); `git-hashing/source-addressing.sh` (`--option substitute true`) |
