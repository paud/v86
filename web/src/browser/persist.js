// IndexedDB-based automatic state persistence for v86
// Saves VM state periodically and on page hide, restores on next load.

const DB_NAME = "v86-persist";
const DB_VERSION = 1;
const STORE_NAME = "states";
const SAVE_INTERVAL_MS = 30 * 1000; // auto-save every 30 seconds

let db = null;
let saveTimer = null;
let isSaving = false;
let lastSaveTime = 0;

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
            console.warn("[v86-persist] Failed to open IndexedDB:", e.target.error);
            reject(e.target.error);
        };
    });
}

/**
 * Save a state ArrayBuffer to IndexedDB under the given key.
 * @param {string} key
 * @param {ArrayBuffer} state
 * @returns {Promise<void>}
 */
export async function saveState(key, state)
{
    if(isSaving) return;
    isSaving = true;
    try
    {
        const d = await openDB();
        await new Promise((resolve, reject) =>
        {
            const tx = d.transaction(STORE_NAME, "readwrite");
            tx.objectStore(STORE_NAME).put(state, key);
            tx.oncomplete = () => resolve();
            tx.onerror = () => reject(tx.error);
        });
        lastSaveTime = Date.now();
        console.log("[v86-persist] State saved (" + (state.byteLength / 1024 / 1024).toFixed(1) + " MB)");
    }
    catch(err)
    {
        console.warn("[v86-persist] Save failed:", err);
    }
    finally
    {
        isSaving = false;
    }
}

/**
 * Load a saved state from IndexedDB.
 * @param {string} key
 * @returns {Promise<ArrayBuffer|null>}
 */
export async function loadState(key)
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
        console.warn("[v86-persist] Load failed:", err);
        return null;
    }
}

/**
 * Delete a saved state from IndexedDB.
 * @param {string} key
 * @returns {Promise<void>}
 */
export async function deleteState(key)
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
        console.log("[v86-persist] Saved state deleted");
    }
    catch(err)
    {
        console.warn("[v86-persist] Delete failed:", err);
    }
}

/**
 * Check whether a saved state exists for the given key.
 * @param {string} key
 * @returns {Promise<boolean>}
 */
export async function hasState(key)
{
    const state = await loadState(key);
    return state !== null;
}

/**
 * Start automatic periodic saving.
 * @param {string} key Storage key (e.g. profile ID)
 * @param {function(): Promise<ArrayBuffer>} saveFn Function that returns the current state
 */
export function startAutoSave(key, saveFn)
{
    stopAutoSave();

    const doSave = async () =>
    {
        try
        {
            const state = await saveFn();
            await saveState(key, state);
        }
        catch(err)
        {
            console.warn("[v86-persist] Auto-save failed:", err);
        }
    };

    // Periodic save
    saveTimer = setInterval(doSave, SAVE_INTERVAL_MS);

    // Save when page is hidden or about to unload
    const onHide = () =>
    {
        if(!document.hidden) return;
        doSave();
    };
    const onBeforeUnload = () =>
    {
        // Use synchronous-ish approach: sendBeacon can't send ArrayBuffer easily,
        // so we just trigger an async save. The browser may or may not complete it.
        doSave();
    };
    document.addEventListener("visibilitychange", onHide);
    window.addEventListener("beforeunload", onBeforeUnload);
    window.addEventListener("pagehide", onBeforeUnload);

    // Store cleanup references
    startAutoSave._cleanup = () =>
    {
        clearInterval(saveTimer);
        document.removeEventListener("visibilitychange", onHide);
        window.removeEventListener("beforeunload", onBeforeUnload);
        window.removeEventListener("pagehide", onBeforeUnload);
    };
}

/**
 * Stop automatic saving.
 */
export function stopAutoSave()
{
    if(saveTimer)
    {
        clearInterval(saveTimer);
        saveTimer = null;
    }
    if(startAutoSave._cleanup)
    {
        startAutoSave._cleanup();
        startAutoSave._cleanup = null;
    }
}
