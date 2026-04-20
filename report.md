# PES-VCS Lab Report

## Phase 5: Branching and Checkout

### Q5.1 — How would `pes checkout <branch>` work?

To implement `pes checkout <branch>`:
1. Read `.pes/HEAD` to find the current branch
2. Check if `.pes/refs/heads/<branch>` exists — if not, error out
3. Read the target commit hash from `.pes/refs/heads/<branch>`
4. Read that commit object to get its tree hash
5. Recursively walk the tree and restore every file to the working directory
6. Write `ref: refs/heads/<branch>` into `.pes/HEAD`

This operation is complex because:
- Files in the current branch but not in the target must be **deleted**
- Files in the target but not current must be **created**
- If any tracked file has uncommitted changes that clash with the target branch, checkout must **refuse** to avoid data loss

### Q5.2 — Detecting dirty working directory conflicts

For every entry in the index:
1. Use `stat()` to get the file's current `mtime` and `size` on disk
2. Compare with the stored `mtime_sec` and `size` in the index entry
3. If they differ, the file is **locally modified** (dirty)
4. Then check if that same file differs between the current HEAD tree and the target branch tree by comparing their blob hashes
5. If both are true (file is dirty AND differs between branches), abort checkout with a conflict error

This works using only the index and object store — no re-hashing needed thanks to the mtime+size fast-check optimization.

### Q5.3 — Detached HEAD

Detached HEAD means `.pes/HEAD` contains a raw commit hash directly instead of `ref: refs/heads/main`.

If you make commits in this state:
- New commit objects are written to the object store correctly
- But no branch file is updated — only `.pes/HEAD` itself gets the new hash
- When you switch to another branch, HEAD changes and the detached commits become **unreachable** — no branch points to them

Recovery:
1. Note the commit hash from `pes log` before switching
2. Create a new branch pointing to it:
echo "<hash>" > .pes/refs/heads/recovered
3. Now that branch preserves the commits permanently

---

## Phase 6: Garbage Collection

### Q6.1 — Algorithm to find and delete unreachable objects

**Algorithm:**
1. Start a `reachable` hash set (e.g. a hash table or sorted array)
2. For every file in `.pes/refs/heads/`, read the commit hash
3. For each commit hash, walk the parent chain:
   - Add the commit hash to `reachable`
   - Add its tree hash to `reachable`
   - Recursively add all blob hashes from that tree
   - Move to the parent commit and repeat
4. Scan every file in `.pes/objects/XX/` directories
5. Any file whose name (reconstructed hash) is NOT in `reachable` → delete it

**Data structure:** A hash set for O(1) lookup per object.

**Estimate for 100,000 commits, 50 branches:**
- Average ~10 objects per commit (1 commit + 1 tree + ~8 blobs)
- Total reachable objects ≈ 1,000,000
- GC must visit all of them → ~1 million object checks

### Q6.2 — Race condition between GC and concurrent commit

**The race:**
1. Thread A (commit) writes a new blob to the object store
2. Thread B (GC) scans objects — sees the blob is not reachable from any ref yet — **deletes it**
3. Thread A writes the commit object pointing to the deleted blob
4. The repo is now **corrupted** — the commit references a missing blob

**How Git avoids this:**
- Git uses a **grace period** — objects newer than 2 weeks are never deleted by GC regardless of reachability
- Git also writes a `.git/gc.pid` lock file so only one GC runs at a time
- The commit operation is designed so blobs are always written before the commit that references them, and refs are only updated last — so a partial commit is always safe to GC after the grace period expires
