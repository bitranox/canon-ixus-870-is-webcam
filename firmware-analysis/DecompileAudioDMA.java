// DecompileAudioDMA.java — Ghidra headless script
// Decompiles the audio command processor FUN_ff842d04 and related functions
// to find where the audio DMA buffer address is programmed.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAudioDMA extends GhidraScript {

    private static final long[] ADDRESSES = {
        0xff842d04L,  // Audio command processor — called by FUN_ff847334
        0xff843000L,  // Nearby — may be audio DMA setup
        0xff843100L,  // Nearby
        0xff843200L,  // Nearby
        0xff843300L,  // Nearby
        0xff843400L,  // Nearby

        // Functions that reference 0xC0220000 (AudioIC) and 0xC0223000 (DMA)
        // Scan the ROM literal pool entries we found:
        0xff8462ecL,  // References 0xC0220000

        // Try to find the audio interrupt handler — look near the DMA registers
        // DryOS interrupt registration: FUN_ff81ae8c seen in JogDial setup
        // Audio may use similar pattern
        0xff846208L,  // Near 0xC02200F8/FC references

        // Functions called during movie recording that set up audio
        // sub_FF85DE1C is the recording start handler (msg 2)
        0xff85de1cL,  // Recording start — may set up audio DMA

        // UIFS_StartMovieRecord
        0xff883d50L,  // UIFS_StartMovieRecord — triggers everything
    };

    private static final String[] NAMES = {
        "FUN_ff842d04 — audio command processor (core)",
        "FUN_ff843000 — audio area",
        "FUN_ff843100 — audio area",
        "FUN_ff843200 — audio area",
        "FUN_ff843300 — audio area",
        "FUN_ff843400 — audio area",
        "FUN_ff8462ec — references 0xC0220000",
        "FUN_ff846208 — near AudioIC register refs",
        "sub_FF85DE1C — recording start (msg 2)",
        "UIFS_StartMovieRecord — recording trigger",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\audio_dma_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Audio DMA Configuration — Decompiled Functions\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Read ROM data values near FUN_ff842d04
        output.append("--- ROM Data Values near audio command processor ---\n");
        for (long a = 0xff842d00L; a < 0xff842d80L; a += 4) {
            Address addr = toAddr(a);
            try {
                int val = mem.getInt(addr);
                if (val != 0 && (
                    (val & 0xFF000000) == 0xC0000000 ||  // I/O register
                    (val > 0x1000 && val < 0x04000000) || // RAM address
                    (val > 0x40000000 && val < 0x44000000) // uncached RAM
                )) {
                    output.append(String.format("  0x%08X = 0x%08X\n", a, val & 0xFFFFFFFFL));
                }
            } catch (Exception e) {}
        }

        // Read ROM data values for FUN_ff847334 params
        output.append("\n--- FUN_ff847334 parameter ROM values ---\n");
        long[] paramAddrs = {
            0xff847404L, 0xff847408L,  // FUN_ff847334 literals
            0xff846894L, 0xff846898L, 0xff84689cL, 0xff8468a0L,  // FUN_ff846764 config params
            0xff8468a4L, 0xff8468a8L, 0xff8468acL, 0xff8468b0L,
            0xff8468b4L, 0xff8468b8L, 0xff8468bcL, 0xff8468c0L,
            0xff8468c4L, 0xff8468c8L, 0xff8468ccL, 0xff8468d0L,
        };
        for (long a : paramAddrs) {
            Address addr = toAddr(a);
            try {
                int val = mem.getInt(addr);
                output.append(String.format("  0x%08X = 0x%08X (%d)\n", a, val & 0xFFFFFFFFL, val));
            } catch (Exception e) {}
        }
        output.append("\n");

        // Decompile functions
        for (int i = 0; i < ADDRESSES.length; i++) {
            Address addr = toAddr(ADDRESSES[i]);
            Function func = fm.getFunctionAt(addr);

            output.append("========================================================================\n");
            output.append(String.format("Function %d/%d: %s\n", i + 1, ADDRESSES.length, NAMES[i]));
            output.append(String.format("Address: 0x%08X\n", ADDRESSES[i]));
            output.append("========================================================================\n");

            if (func == null) {
                func = createFunction(addr, null);
            }

            if (func != null) {
                output.append("Ghidra name: " + func.getName() + "\n");
                output.append("Size: " + func.getBody().getNumAddresses() + " bytes\n");

                DecompileResults results = decomp.decompileFunction(func, 30, monitor);
                if (results.decompileCompleted()) {
                    output.append("\n" + results.getDecompiledFunction().getC() + "\n\n");
                } else {
                    output.append("\n[DECOMPILATION FAILED]\n\n");
                }
            } else {
                output.append("[NO FUNCTION AT THIS ADDRESS]\n\n");
            }
        }

        // Scan for I/O register references in audio command processor area
        output.append("\n========================================================================\n");
        output.append("I/O Register References (0xFF842D00-0xFF843500)\n");
        output.append("========================================================================\n\n");
        for (long a = 0xff842d00L; a < 0xff843500L; a += 4) {
            Address addr = toAddr(a);
            try {
                int val = mem.getInt(addr);
                if ((val & 0xFF000000) == 0xC0000000) {
                    output.append(String.format("  0x%08X: 0x%08X\n", a, val & 0xFFFFFFFFL));
                }
            } catch (Exception e) {}
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("Audio DMA decompilation written to: " + OUTPUT_FILE);
    }
}
