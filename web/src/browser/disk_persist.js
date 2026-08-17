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
    if(db) return Promise.resolve(db);

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
 * @param {Object} buffer
 * @param {Array<[number, Uint8Array]>} blocks
 */
export function applyDirtyBlocks(buffer, blocks)
{
    if(!buffer || !blocks || !buffer.block_cache || !buffer.block_cache_is_write)
    {
        return false;
    }

    for(const [index, block] of blocks)
    {
        buffer.block_cache.set(index, block);
        buffer.block_cache_is_write.add(index);
    }
    return true;
}

/**
 * Save dirty blocks for a disk to IndexedDB.
 * @param {string} key Disk identifier, e.g. "windows98-hda"
 * @param {Array<[number, Uint8Array]>} blocks
 */
export async function saveDirtyBlocks(key, blocks)
{
    try
    {
        const d = await openDB();
        await new Promise((resolve, reject) =>
        {
            const tx = d.transaction(STORE_NAME, "readwrite");
            tx.objectStore(STORE_NAME).put(blocks, key);
            tx.oncomplete = () => resolve();
            tx.onerror = () => reject(tx.error);
        });
        const totalBytes = blocks.reduce((sum, [, b]) => sum + b.byteLength, 0);
        console.log("[v86-disk] Saved " + blocks.length + " dirty blocks (" +
            (totalBytes / 1024).toFixed(0) + " KB) for " + key);
        return true;
    }
    catch(err)
    {
        console.warn("[v86-disk] Save failed for " + key + ":", err);
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
