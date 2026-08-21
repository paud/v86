// Create a CD ISO image with opengl32.dll, ddraw.dll, and install.bat
// for transferring to Win98/XP guest
import fs from "node:fs";
import { generate } from "../src/iso9660.js";

const isoPath = process.argv[2] || "web/images/v86gl_install.iso";

const files = [];
for (const name of ["opengl32.dll", "ddraw.dll"]) {
    const path = `bin/${name}`;
    if (fs.existsSync(path)) {
        files.push({ name: name.toUpperCase(), contents: fs.readFileSync(path) });
    }
}

// install.bat lives in tools/ and is included on the CD as INSTALL.BAT
// (8.3 name, no hierarchy - iso9660.js limitation)
if (fs.existsSync("tools/install.bat")) {
    files.push({ name: "INSTALL.BAT", contents: fs.readFileSync("tools/install.bat") });
}

const iso = generate(files);
fs.writeFileSync(isoPath, iso);
console.log(`ISO written: ${iso.length} bytes to ${isoPath} (${files.map(f => f.name).join(", ")})`);
