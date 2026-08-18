/**
 * Disk-increment persistence for v86.
 *
 * Instead of saving/restoring full VM snapshots, this module persists only
 * the dirty disk blocks (the copy-on-write overlay) to IndexedDB. On page
 * load, the blocks are restored into the disk buffer's block_cache, and the
 * VM always cold-boots from BIOS. The base image continues to be streamed
 * from the server on demand.
 *
 * This gives the machine "real power" semantics:
 *   - Power off: flush dirty blocks to IndexedDB, reset all state
 *   - Power on: cold boot from BIOS; disk reads hit cached dirty blocks
 *     first, then the server's base image
 */

const DB_NAME = "v86-disks";
const DB_VERSION = 1;
const STORE_NAME = "disks";

let db = null;

function openDB()
{
    if(db)
    {
        // Check if the connection is still valid
        if(db.closePending || db.name === "")
        {
            db = null;
        }
        else
        {
            return Promise.resolve(db);
        }
    }

    return new Promise((resolve, reject) =>
    {
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = function(e)
        {
            const d = e.target.result;
            if(!d.objectStoreNames.contains(STORE_NAME))
            {
                d.createObjectStore(STORE_NAME);
            }
        };
        req.onsuccess = function(e)
        {
            db = e.target.result;
            // If the database connection gets closed externally, clear cache
            db.onclose = () => { db = null; };
            db.onversionchange = () => { db.close(); db = null; };
            resolve(db);
        };
        req.onerror = function(e)
        {
            console.warn("[v86-disk] Failed to open IndexedDB:", e.target.error);
            reject(e.target.error);
        };
    });
}

/**
 * Extract dirty blocks from a disk buffer (IDE/FDC/CDROM buffer object).
 * Returns an array of [block_index, Uint8Array] pairs, or null if the
 * buffer has no accessible dirty-block state.
 * @param {Object} buffer A v86 disk buffer object
 * @returns {Array<[number, Uint8Array]>|null}
 */
export function extractDirtyBlocks(buffer)
{
    if(!buffer) return null;

    if(buffer.block_cache && buffer.block_cache_is_write)
    {
        // AsyncXHRBuffer / AsyncXHRPartfileBuffer / AsyncFileBuffer
        const blocks = [];
        for(const [index, block] of buffer.block_cache)
        {
            if(buffer.block_cache_is_write.has(index))
            {
                blocks.push([index, block.slice()]);
            }
        }
        console.log("[v86-disk] extractDirtyBlocks: " + blocks.length +
            " dirty / " + buffer.block_cache.size + " cached blocks");
        return blocks;
    }

    if(buffer.get_state)
    {
        // Fall back to get_state() for SyncBuffer-like objects; returns
        // the whole buffer. Callers should avoid this for large disks.
        return null;
    }

    return null;
}

/**
 * Apply saved dirty blocks to a disk buffer.
 * Existing blocks in the cache are NOT overwritten (state blocks take priority).
 * @param {Object} buffer
 * @param {Array<[number, Uint8Array]>} blocks
 * @param {boolean} overwrite Whether to overwrite existing cached blocks
 */
export function applyDirtyBlocks(buffer, blocks, overwrite = false)
{
    if(!buffer || !blocks || !buffer.block_cache || !buffer.block_cache_is_write)
    {
        return false;
    }

    let applied = 0;
    let skipped = 0;
    for(const [index, block] of blocks)
    {
        if(!overwrite && buffer.block_cache.has(index))
        {
            // Block already in cache (e.g., from a loaded state) — keep it
            skipped++;
            continue;
        }
        buffer.block_cache.set(index, block);
        buffer.block_cache_is_write.add(index);
        applied++;
    }
    if(applied || skipped)
    {
        console.log("[v86-disk] applyDirtyBlocks: " + applied + " applied, " +
            skipped + " skipped (already in cache)");
    }
    return true;
}

/**
 * Re-apply IndexedDB blocks to a disk buffer after a state restore.
 * State blocks already in the cache take priority; IndexedDB blocks
 * fill in the gaps so no persisted changes are lost.
 * @param {string} key
 * @param {Object} buffer
 */
export async function reapplyAfterStateRestore(key, buffer)
{
    if(!buffer || !buffer.block_cache) return;

    const saved = await loadDirtyBlocks(key);
    if(!saved || !saved.length) return;

    console.log("[v86-disk] Re-applying " + saved.length +
        " persisted blocks after state restore for " + key);
    applyDirtyBlocks(buffer, saved, false);
}

/**
 * Merge two dirty-block arrays. Newer blocks take priority on conflict.
 * @param {Array<[number, Uint8Array]>|null} existing
 * @param {Array<[number, Uint8Array]>} incoming
 * @returns {Array<[number, Uint8Array]>}
 */
function mergeBlocks(existing, incoming)
{
    if(!existing || !existing.length) return incoming;
    const map = new Map();
    for(const [index, block] of existing)
    {
        map.set(index, block);
    }
    for(const [index, block] of incoming)
    {
        map.set(index, block);
    }
    return Array.from(map.entries());
}

/**
 * Save dirty blocks for a disk to IndexedDB.
 * Merges with any previously saved blocks so that no changes are lost.
 * @param {string} key Disk identifier, e.g. "windows98-hda"
 * @param {Array<[number, Uint8Array]>} blocks
 */
export async function saveDirtyBlocks(key, blocks, retries = 2)
{
    try
    {
        const d = await openDB();

        // Load existing blocks and merge (new blocks take priority)
        const existing = await new Promise((resolve, reject) =>
        {
            const tx = d.transaction(STORE_NAME, "readonly");
            const req = tx.objectStore(STORE_NAME).get(key);
            req.onsuccess = () => resolve(req.result || null);
            req.onerror = () => reject(req.error);
        });

        const merged = mergeBlocks(existing, blocks);

        await new Promise((resolve, reject) =>
        {
            const tx = d.transaction(STORE_NAME, "readwrite");
            tx.objectStore(STORE_NAME).put(merged, key);
            tx.oncomplete = () => resolve();
            tx.onerror = () => reject(tx.error);
            tx.onabort = () => reject(tx.error || new Error("transaction aborted"));
        });
        const totalBytes = merged.reduce((sum, [, b]) => sum + b.byteLength, 0);
        const newCount = merged.length - (existing ? existing.length : 0);
        console.log("[v86-disk] Saved " + merged.length + " dirty blocks (" +
            (totalBytes / 1024).toFixed(0) + " KB, +" + newCount + " new) for " + key);
        return true;
    }
    catch(err)
    {
        console.warn("[v86-disk] Save failed for " + key + ":", err);
        // If the connection is closing, clear cache and retry
        if(retries > 0 && (err.name === "InvalidStateError" || err.name === "TransactionInactiveError"))
        {
            db = null;
            return saveDirtyBlocks(key, blocks, retries - 1);
        }
        return false;
    }
}

/**
 * Load dirty blocks for a disk from IndexedDB.
 * @param {string} key
 * @returns {Promise<Array<[number, Uint8Array]>|null>}
 */
export async function loadDirtyBlocks(key)
{
    try
    {
        const d = await openDB();
        return await new Promise((resolve, reject) =>
        {
            const tx = d.transaction(STORE_NAME, "readonly");
            const req = tx.objectStore(STORE_NAME).get(key);
            req.onsuccess = () => resolve(req.result || null);
            req.onerror = () => reject(req.error);
        });
    }
    catch(err)
    {
        console.warn("[v86-disk] Load failed for " + key + ":", err);
        return null;
    }
}

/**
 * Delete saved dirty blocks for a disk.
 * @param {string} key
 */
export async function deleteDirtyBlocks(key)
{
    try
    {
        const d = await openDB();
        await new Promise((resolve, reject) =>
        {
            const tx = d.transaction(STORE_NAME, "readwrite");
            tx.objectStore(STORE_NAME).delete(key);
            tx.oncomplete = () => resolve();
            tx.onerror = () => reject(tx.error);
        });
        console.log("[v86-disk] Deleted dirty blocks for " + key);
        return true;
    }
    catch(err)
    {
        console.warn("[v86-disk] Delete failed:", err);
        return false;
    }
}
