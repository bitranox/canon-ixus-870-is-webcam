// DecompileSIODMA.java — Ghidra headless script
// Deep RE: find the SIO interrupt handler and audio DMA path.
//
// Strategy:
// 1. Find all functions that READ from 0xC0220080 (SIO data register)
// 2. Find all interrupt registrations (FUN_ff81ae8c calls)
// 3. Find functions that reference both 0xC0220080 and RAM write operations
// 4. Decompile the SIO data path to find where PCM samples are accumulated

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileSIODMA extends GhidraScript {

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\sio_dma_deep_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Deep SIO/Audio DMA RE — Find PCM Sample Accumulation Path\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Step 1: Scan ROM for references to 0xC0220080 (SIO data register)
        // Look for LDR instructions that load this address
        output.append("=== References to 0xC0220080 (SIO data register) ===\n\n");
        int sioRefs = 0;
        for (long a = 0xFF800000L; a < 0xFFFFF000L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if (val == 0xC0220080) {
                    // Found literal pool entry with SIO data register address
                    // Find which function uses this literal
                    Function func = fm.getFunctionContaining(toAddr(a));
                    String funcName = func != null ? func.getName() + " @ " + func.getEntryPoint() : "unknown";
                    output.append(String.format("  ROM 0x%08X contains 0xC0220080 (in %s)\n", a, funcName));
                    sioRefs++;

                    // Decompile the containing function
                    if (func != null && sioRefs <= 8) {
                        DecompileResults results = decomp.decompileFunction(func, 30, monitor);
                        if (results.decompileCompleted()) {
                            output.append("  --- Decompiled: " + func.getName() + " ---\n");
                            output.append(results.getDecompiledFunction().getC() + "\n\n");
                        }
                    }
                }
            } catch (Exception e) {}
        }
        output.append(String.format("\nTotal references to 0xC0220080: %d\n\n", sioRefs));

        // Step 2: Also scan for 0xC0220050 (SIO channel 1 data register)
        output.append("=== References to 0xC0220050 (SIO ch1 data register) ===\n\n");
        int sio1Refs = 0;
        for (long a = 0xFF800000L; a < 0xFFFFF000L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if (val == 0xC0220050) {
                    Function func = fm.getFunctionContaining(toAddr(a));
                    String funcName = func != null ? func.getName() + " @ " + func.getEntryPoint() : "unknown";
                    output.append(String.format("  ROM 0x%08X contains 0xC0220050 (in %s)\n", a, funcName));
                    sio1Refs++;
                }
            } catch (Exception e) {}
        }
        output.append(String.format("\nTotal references to 0xC0220050: %d\n\n", sio1Refs));

        // Step 3: Scan for interrupt registration (FUN_ff81ae8c)
        // This function registers interrupt handlers
        output.append("=== Interrupt registrations near AudioIC code ===\n\n");
        Address intRegAddr = toAddr(0xFF81AE8CL);
        Function intRegFunc = fm.getFunctionAt(intRegAddr);
        if (intRegFunc != null) {
            // Find all callers of the interrupt registration function
            java.util.Set<Function> callers = intRegFunc.getCallingFunctions(monitor);
            for (Function caller : callers) {
                long entry = caller.getEntryPoint().getOffset();
                // Focus on AudioIC area (0xFF842000-0xFF848000)
                if (entry >= 0xFF842000L && entry <= 0xFF848000L) {
                    output.append(String.format("  Audio-area ISR registration: %s @ 0x%08X\n",
                        caller.getName(), entry));
                    DecompileResults results = decomp.decompileFunction(caller, 30, monitor);
                    if (results.decompileCompleted()) {
                        output.append(results.getDecompiledFunction().getC() + "\n\n");
                    }
                }
            }
        }

        // Step 4: Scan for 0xC022xxxx references in the SIO driver area
        output.append("=== All 0xC022xxxx register references in audio code ===\n\n");
        for (long a = 0xFF842000L; a < 0xFF848000L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if ((val & 0xFFFF0000) == 0xC0220000) {
                    Function func = fm.getFunctionContaining(toAddr(a));
                    String funcName = func != null ? func.getName() + " @ " + func.getEntryPoint() : "unknown";
                    output.append(String.format("  0x%08X: 0x%08X (%s)\n", a, val & 0xFFFFFFFFL, funcName));
                }
            } catch (Exception e) {}
        }

        // Write output
        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("Deep SIO/DMA decompilation written to: " + OUTPUT_FILE);
    }
}
