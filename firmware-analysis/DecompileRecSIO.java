// DecompileRecSIO.java — Ghidra headless script
// Force-create and decompile functions near the recording-area SIO driver
// literal pool at 0xFF8C0F50 (contains 0xC0224000).
// Also scan the 0xFF8C0800-0xFF8C1000 range for function prologues.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileRecSIO extends GhidraScript {

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\rec_sio_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Recording-Area SIO Driver — Audio Data Path\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Step 1: Scan for ARM function prologues (STMFD SP!, {...,LR})
        // in the 0xFF8C0800-0xFF8C1000 range
        output.append("=== Function prologue scan (0xFF8C0800-0xFF8C1000) ===\n\n");
        for (long a = 0xFF8C0800L; a < 0xFF8C1000L; a += 4) {
            try {
                int inst = mem.getInt(toAddr(a));
                // STMFD SP!, {...,LR} = 0xE92D4xxx or 0xE92Dxxxx with bit 14 set
                if ((inst & 0xFFFF0000) == 0xE92D0000 && (inst & 0x4000) != 0) {
                    output.append(String.format("  0x%08X: STMFD SP!, {...,LR} = 0x%08X\n",
                        a, inst & 0xFFFFFFFFL));

                    // Try to create function here
                    Function func = fm.getFunctionAt(toAddr(a));
                    if (func == null) {
                        func = createFunction(toAddr(a), null);
                        if (func != null) {
                            output.append(String.format("    Created function: %s (size=%d)\n",
                                func.getName(), func.getBody().getNumAddresses()));
                        }
                    } else {
                        output.append(String.format("    Existing function: %s (size=%d)\n",
                            func.getName(), func.getBody().getNumAddresses()));
                    }
                }
                // Also check for MOV R0, #0 (0xE3A00000) which sometimes starts functions
            } catch (Exception e) {}
        }
        output.append("\n");

        // Step 2: Decompile all functions found in this range
        output.append("=== Decompiled functions ===\n\n");
        int count = 0;
        for (long a = 0xFF8C0800L; a < 0xFF8C1000L && count < 15; a += 4) {
            Function func = fm.getFunctionAt(toAddr(a));
            if (func == null) continue;

            output.append("========================================================================\n");
            output.append(String.format("%s @ 0x%08X (size=%d)\n",
                func.getName(), a, func.getBody().getNumAddresses()));
            output.append("========================================================================\n");

            DecompileResults results = decomp.decompileFunction(func, 30, monitor);
            if (results.decompileCompleted()) {
                output.append(results.getDecompiledFunction().getC() + "\n\n");
            } else {
                output.append("[DECOMPILATION FAILED]\n\n");
            }
            count++;
        }

        // Step 3: Also read ALL ROM data around 0xFF8C0F00-0xFF8C1060
        // to understand the complete literal pool and find buffer addresses
        output.append("========================================================================\n");
        output.append("Full literal pool / data area (0xFF8C0F00-0xFF8C1060)\n");
        output.append("========================================================================\n\n");
        for (long a = 0xFF8C0F00L; a < 0xFF8C1060L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if (val != 0) {
                    output.append(String.format("  0x%08X = 0x%08X", a, val & 0xFFFFFFFFL));
                    if ((val & 0xFF000000) == 0xC0000000) output.append(" (I/O)");
                    else if (val > 0xFF800000L) output.append(" (ROM)");
                    else if (val > 0x1000 && val < 0x04000000) output.append(" (RAM)");
                    // Check if it's ASCII
                    byte b0 = (byte)(val & 0xFF);
                    byte b1 = (byte)((val >> 8) & 0xFF);
                    byte b2 = (byte)((val >> 16) & 0xFF);
                    byte b3 = (byte)((val >> 24) & 0xFF);
                    if (b0 >= 0x20 && b0 < 0x7F && b1 >= 0x20 && b1 < 0x7F) {
                        output.append(String.format(" '%c%c%c%c'",
                            (char)b0, (char)b1, b2 >= 0x20 ? (char)b2 : '.', b3 >= 0x20 ? (char)b3 : '.'));
                    }
                    output.append("\n");
                }
            } catch (Exception e) {}
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("Recording-area SIO decompilation written to: " + OUTPUT_FILE);
    }
}
