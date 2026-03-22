// DecompileAudioFIFO.java — Ghidra headless script
// Decompile functions that reference 0xC022F000 and 0xC0224000 —
// suspected audio FIFO/DMA hardware registers for PCM data path.
// Also scan the broader 0xFF843000-0xFF846000 area for audio DMA functions.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;

public class DecompileAudioFIFO extends GhidraScript {

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\audio_fifo_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Audio FIFO/DMA Hardware — Decompiled Functions\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Step 1: Find ALL functions that contain references to audio-specific registers
        // 0xC022F000, 0xC0224000, and also scan for potential DMA buffer setup
        output.append("=== Scanning ROM for audio FIFO/DMA register references ===\n\n");

        LinkedHashSet<Long> funcAddrs = new LinkedHashSet<>();

        // Scan for 0xC022Fxxx, 0xC0224xxx, 0xC0225xxx references
        for (long a = 0xFF800000L; a < 0xFFFFF000L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if ((val & 0xFFFFF000) == 0xC022F000 ||
                    (val & 0xFFFFF000) == 0xC0224000 ||
                    (val & 0xFFFFF000) == 0xC0225000) {
                    Function func = fm.getFunctionContaining(toAddr(a));
                    String funcName = func != null ?
                        func.getName() + " @ " + func.getEntryPoint() : "data/unknown";
                    output.append(String.format("  0x%08X = 0x%08X (%s)\n",
                        a, val & 0xFFFFFFFFL, funcName));
                    if (func != null) {
                        funcAddrs.add(func.getEntryPoint().getOffset());
                    }
                }
            } catch (Exception e) {}
        }
        output.append(String.format("\nUnique functions found: %d\n\n", funcAddrs.size()));

        // Step 2: Read the register values at these addresses to understand layout
        output.append("=== Audio FIFO Register Map ===\n\n");
        output.append("0xC022F000 range:\n");
        for (long reg = 0xC022F000L; reg <= 0xC022F030L; reg += 4) {
            // Can't read I/O regs from Ghidra, but list what we know from ROM refs
        }

        output.append("0xC0224000 range:\n");

        // Step 3: Decompile all unique functions that reference these registers
        int count = 0;
        int uncovered = 0;
        for (long addr : funcAddrs) {
            if (count >= 15) break;
            Function func = fm.getFunctionAt(toAddr(addr));
            if (func == null) continue;

            output.append("========================================================================\n");
            output.append(String.format("Function: %s @ 0x%08X (size=%d bytes)\n",
                func.getName(), addr, func.getBody().getNumAddresses()));
            output.append("========================================================================\n");

            // List calls and callers
            java.util.Set<Function> callers = func.getCallingFunctions(monitor);
            if (!callers.isEmpty()) {
                output.append("Called by:\n");
                for (Function c : callers) {
                    output.append(String.format("  <- %s @ %s\n", c.getName(), c.getEntryPoint()));
                }
            }
            java.util.Set<Function> callees = func.getCalledFunctions(monitor);
            if (!callees.isEmpty()) {
                output.append("Calls:\n");
                for (Function c : callees) {
                    output.append(String.format("  -> %s @ %s\n", c.getName(), c.getEntryPoint()));
                }
            }

            DecompileResults results = decomp.decompileFunction(func, 30, monitor);
            if (results.decompileCompleted()) {
                output.append("\n" + results.getDecompiledFunction().getC() + "\n\n");
            } else {
                output.append("\n[DECOMPILATION FAILED]\n\n");
            }
            count++;
        }

        // Step 4: Also decompile functions in the 0xFF843800-0xFF845800 range
        // that we haven't covered — these are between SIO driver and AudioIC
        output.append("\n========================================================================\n");
        output.append("Uncovered functions in audio driver area (0xFF843800-0xFF845800)\n");
        output.append("========================================================================\n\n");

        // Iterate functions in the audio driver area
        for (long faddr = 0xFF843800L; faddr < 0xFF845800L && uncovered < 10; faddr += 4) {
            Function f2 = fm.getFunctionAt(toAddr(faddr));
            if (f2 == null) continue;
            if (funcAddrs.contains(faddr)) continue;
            if (f2.getBody().getNumAddresses() <= 20) continue;

            output.append(String.format("--- %s @ 0x%08X (size=%d) ---\n",
                f2.getName(), faddr, f2.getBody().getNumAddresses()));
            DecompileResults results = decomp.decompileFunction(f2, 30, monitor);
            if (results.decompileCompleted()) {
                String code = results.getDecompiledFunction().getC();
                if (code.contains("0xc0") || code.contains("DAT_") || code.length() > 200) {
                    output.append(code + "\n\n");
                    uncovered++;
                }
            }
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("Audio FIFO decompilation written to: " + OUTPUT_FILE);
    }
}
