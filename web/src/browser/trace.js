/**
 * Lightweight tracing system for v86.
 * Logs component lifecycle events with timestamps to the console.
 *
 * Usage:
 *   import { trace } from "./trace.js";
 *   trace("CPU", "starting");
 *   trace("VGA", "mode set", { width: 80, height: 25 });
 */

const COLORS = {
    "CPU":      "#007700",
    "VGA":      "#0066cc",
    "IDE":      "#cc6600",
    "FDC":      "#9933cc",
    "PS2":      "#cc0066",
    "PIC":      "#666666",
    "PIT":      "#666666",
    "DMA":      "#666666",
    "RTC":      "#666666",
    "ACPI":     "#009999",
    "PCI":      "#666666",
    "Net":      "#cc0000",
    "SB16":     "#996633",
    "Virtio":   "#336699",
    "Starter":  "#000000",
    "Main":     "#000000",
    "Persist":  "#7700aa",
    "Screen":   "#0066cc",
    "Keyboard": "#cc0066",
    "Mouse":    "#cc0066",
    "Serial":   "#009933",
    "Speaker":  "#996633",
    "BIOS":     "#000000",
    "State":    "#7700aa",
    "Bus":      "#666666",
};

const START_TIME = performance.now();

let enabled = true;

/**
 * Enable or disable tracing.
 * @param {boolean} on
 */
export function set_trace(on)
{
    enabled = !!on;
}

/**
 * @return {boolean}
 */
export function is_trace_enabled()
{
    return enabled;
}

/**
 * Log a trace event.
 * @param {string} component Component name (e.g. "CPU", "VGA")
 * @param {string} event Event description
 * @param {Object=} data Optional data object
 */
export function trace(component, event, data)
{
    if(!enabled) return;

    const elapsed = ((performance.now() - START_TIME) / 1000).toFixed(3);
    const color = COLORS[component] || "#333333";

    const label = `%c[v86 ${elapsed}s] %c${component}%c ${event}`;
    const styles = [
        "color: #888",
        `color: ${color}; font-weight: bold`,
        "color: #000",
    ];

    if(data !== undefined)
    {
        console.log(label, ...styles, data);
    }
    else
    {
        console.log(label, ...styles);
    }
}

/**
 * Trace an async operation with timing.
 * @param {string} component
 * @param {string} event
 * @param {function(): Promise<T>} fn
 * @return {Promise<T>}
 * @template T
 */
export async function trace_async(component, event, fn)
{
    trace(component, event + " start");
    const t0 = performance.now();
    try
    {
        const result = await fn();
        const ms = (performance.now() - t0).toFixed(1);
        trace(component, event + " done (" + ms + "ms)");
        return result;
    }
    catch(err)
    {
        const ms = (performance.now() - t0).toFixed(1);
        trace(component, event + " FAILED (" + ms + "ms)", err);
        throw err;
    }
}

/**
 * Wrap a method with tracing.
 * @param {Object} obj
 * @param {string} method
 * @param {string} component
 * @param {string=} event
 */
export function trace_method(obj, method, component, event)
{
    const original = obj[method];
    const name = event || method;
    obj[method] = function(...args)
    {
        trace(component, name, args.length < 3 ? args : undefined);
        return original.apply(this, args);
    };
}
