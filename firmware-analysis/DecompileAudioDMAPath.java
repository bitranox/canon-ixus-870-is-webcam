// DecompileAudioDMAPath.java — Ghidra headless script
// Follow the 0xC0224000 reference at 0xFF8C0F50 (recording area)
// and the 0xC022F000 references to find the audio DMA data path.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAudioDMAPath extends GhidraScript {

    // Functions near the 0xC0224000 and 0xC022F000 literal pool entries
    private static final long[] ADDRESSES = {
        // Near 0xFF8C0F50 (contains 0xC0224000) — recording area
        0xff8c0e00L,
        0xff8c0f00L,
        0xff8c1000L,
        0xff8c1100L,

        // Near 0xFF84391C (contains 0xC022F000) — ADC/audio area
        0xff8436a0L,  // IRQ 0x40 handler (registered by FUN_ff843800)
        0xff84372cL,  // IRQ 0x80-0x89 handler (DMA completion?)
        0xff8432a8L,  // Called by FUN_ff843800 at start
        0xff8432bcL,  // Called by FUN_ff843800

        // Near 0xFF86C5D8 (contains 0xC022F000) — another area
        0xff86c500L,
        0xff86c5d0L,

        // Near 0xFFA3DA54 (contains 0xC0224000)
        0xffa3da00L,

        // Near 0xFFFF04FC / 0xFFFF0578 (kernel area, 0xC022F000/F200/F204)
        // These might be the DryOS kernel's DMA/interrupt handler mappings
    };

    private static final String[] NAMES = {
        "Near 0xC0224000 ref (0xFF8C0E00) — recording area",
        "Near 0xC0224000 ref (0xFF8C0F00) — recording area",
        "Near 0xC0224000 ref (0xFF8C1000) — recording area",
        "Near 0xC0224000 ref (0xFF8C1100) — recording area",

        "FUN_ff8436a0 — IRQ 0x40 handler",
        "FUN_ff84372c — IRQ 0x80-0x89 handler (DMA completion?)",
        "FUN_ff8432a8 — called by DMA init",
        "FUN_ff8432bc — called by DMA init",

        "Near 0xC022F000 ref (0xFF86C500)",
        "Near 0xC022F000 ref (0xFF86C5D0)",

        "Near 0xC0224000 ref (0xFFA3DA00)",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\audio_dma_path_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Audio DMA Path — Following 0xC0224000 and 0xC022F000 References\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Read ROM constants near the literal pool entries
        output.append("=== Literal pool context ===\n\n");
        long[] litAddrs = {0xFF8C0F40L, 0xFF8C0F44L, 0xFF8C0F48L, 0xFF8C0F4CL,
                           0xFF8C0F50L, 0xFF8C0F54L, 0xFF8C0F58L, 0xFF8C0F5CL};
        for (long a : litAddrs) {
            try {
                int val = mem.getInt(toAddr(a));
                output.append(String.format("  0x%08X = 0x%08X\n", a, val & 0xFFFFFFFFL));
            } catch (Exception e) {}
        }
        output.append("\n");

        // Decompile functions
        for (int i = 0; i < ADDRESSES.length; i++) {
            Address addr = toAddr(ADDRESSES[i]);
            Function func = fm.getFunctionAt(addr);

            // If no function at exact address, try nearby
            if (func == null) {
                for (int delta = -16; delta <= 16; delta += 4) {
                    func = fm.getFunctionAt(toAddr(ADDRESSES[i] + delta));
                    if (func != null) break;
                }
            }

            output.append("========================================================================\n");
            output.append(String.format("%s\n", NAMES[i]));
            if (func != null) {
                output.append(String.format("Found: %s @ 0x%s (size=%d)\n",
                    func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses()));

                DecompileResults results = decomp.decompileFunction(func, 30, monitor);
                if (results.decompileCompleted()) {
                    output.append("\n" + results.getDecompiledFunction().getC() + "\n\n");
                } else {
                    output.append("\n[DECOMPILATION FAILED]\n\n");
                }
            } else {
                output.append(String.format("No function found near 0x%08X\n\n", ADDRESSES[i]));
            }
        }

        // Also read the kernel area entries
        output.append("========================================================================\n");
        output.append("Kernel DMA/interrupt table (0xFFFF0400-0xFFFF0600)\n");
        output.append("========================================================================\n\n");
        for (long a = 0xFFFF0400L; a < 0xFFFF0600L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if ((val & 0xFFFFF000) == 0xC0220000 ||
                    (val & 0xFFFFF000) == 0xC0224000 ||
                    (val & 0xFFFFF000) == 0xC022F000 ||
                    (val > 0xFF800000L && val != 0xFFFFFFFF)) {
                    output.append(String.format("  0x%08X = 0x%08X\n", a, val & 0xFFFFFFFFL));
                }
            } catch (Exception e) {}
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("Audio DMA path decompilation written to: " + OUTPUT_FILE);
    }
}
