# Ghidra headless script to decompile hardware MJPEG functions
# Run with: analyzeHeadless <project_dir> <project_name> -process PRIMARY.BIN -postScript decompile_mjpeg.py
#
# This script decompiles the key firmware functions used by the webcam module's
# hardware MJPEG encoding path and writes the output to a text file.

#@category CHDK
#@author webcam-re

import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Target functions to decompile (address, name)
TARGET_FUNCTIONS = [
    (0xFF9E8DD8, "StartMjpegMaking_FW"),
    (0xFF9E8DF8, "StopMjpegMaking_FW"),
    (0xFFAA234C, "GetContinuousMovieJpegVRAMData_FW"),
    (0xFF8C4178, "GetMovieJpegVRAMHPixelsSize_FW"),
    (0xFF8C4184, "GetMovieJpegVRAMVPixelsSize_FW"),
    (0xFF8C425C, "StopContinuousVRAMData_FW"),
    (0xFF9E8944, "StartEVFMovVGA_FW"),
]

# Additional context functions
CONTEXT_FUNCTIONS = [
    (0xFF9E8A24, "StartEVFMovQVGA60_FW"),
    (0xFF9E8C58, "StartEVFMovXGA_FW"),
    (0xFF9E8D10, "StartEVFMovHD_FW"),
    (0xFF9E8DC8, "StopEVF_FW"),
]

def decompile_function(decomp, addr, name, monitor):
    """Decompile a function at the given address and return the C code."""
    func = getFunctionAt(toAddr(addr))
    if func is None:
        # Try to create the function
        func = createFunction(toAddr(addr), name)
    if func is None:
        return "// Could not find or create function %s at 0x%08X\n" % (name, addr)

    results = decomp.decompileFunction(func, 60, monitor)
    if results is None or not results.decompileCompleted():
        return "// Decompilation failed for %s at 0x%08X\n" % (name, addr)

    decomp_func = results.getDecompiledFunction()
    if decomp_func is None:
        return "// No decompiled output for %s at 0x%08X\n" % (name, addr)

    sig = decomp_func.getSignature()
    code = results.getDecompiledFunction().getC()

    return "// === %s @ 0x%08X ===\n// Signature: %s\n%s\n" % (name, addr, sig, code)

def main():
    monitor = ConsoleTaskMonitor()

    # Set up decompiler
    decomp = DecompInterface()
    decomp.openProgram(currentProgram)

    output_path = os.path.join(
        os.path.dirname(getSourceFile().getAbsolutePath()),
        "mjpeg_decompiled.txt"
    )

    output = []
    output.append("=" * 72)
    output.append("Hardware MJPEG Function Decompilation")
    output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a")
    output.append("=" * 72)
    output.append("")

    output.append("=" * 72)
    output.append("PRIMARY TARGET FUNCTIONS")
    output.append("=" * 72)
    output.append("")

    for addr, name in TARGET_FUNCTIONS:
        print("Decompiling %s at 0x%08X..." % (name, addr))
        code = decompile_function(decomp, addr, name, monitor)
        output.append(code)

    output.append("=" * 72)
    output.append("CONTEXT FUNCTIONS (for cross-reference)")
    output.append("=" * 72)
    output.append("")

    for addr, name in CONTEXT_FUNCTIONS:
        print("Decompiling %s at 0x%08X..." % (name, addr))
        code = decompile_function(decomp, addr, name, monitor)
        output.append(code)

    # Write output
    result = "\n".join(output)
    with open(output_path, "w") as f:
        f.write(result)

    print("\nDecompilation output written to: %s" % output_path)
    print("\n" + result)

main()
