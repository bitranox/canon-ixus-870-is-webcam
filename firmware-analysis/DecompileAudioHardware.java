// DecompileAudioHardware.java — Ghidra headless script
// Decompiles AudioIC hardware layer to find live PCM DMA buffer addresses.
//
// task_AudioTsk calls FUN_ff847334 three times to configure audio hardware.
// The AudioIC WM1400 uses DMA to write PCM samples to a RAM buffer.
// We need to find that buffer address and the read/write pointers.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.Set;
import java.util.LinkedHashSet;

public class DecompileAudioHardware extends GhidraScript {

    private static final long[] ADDRESSES = {
        // AudioTsk hardware config functions
        0xff847334L,  // FUN_ff847334 — called 3x by task_AudioTsk (audio HW config)
        0xff8465ccL,  // task_AudioTsk (re-decompile for full call chain)

        // AudioIC driver layer (from funcs_by_name: AudioIC_WM1400.c)
        0xff846624L,  // Near task_AudioTsk — likely AudioIC init/setup
        0xff846764L,  // Near task_AudioTsk — likely AudioIC DMA setup
        0xff8467a8L,  // Near task_AudioTsk — AudioIC close/cleanup

        // DMA and interrupt handlers for audio
        0xff84c170L,  // Referenced by StartSoundRecord — resource lookup
        0xff84c144L,  // Referenced by StartSoundRecord — resource register
        0xff84c118L,  // Referenced by StartSoundRecord — resource release

        // Audio hardware register access (from AudioIC_WM1400.c vicinity)
        0xff846200L,  // AudioIC area — potential DMA buffer setup
        0xff846300L,  // AudioIC area — potential interrupt handler
        0xff846400L,  // AudioIC area — potential buffer management
        0xff846500L,  // AudioIC area — near task_AudioTsk

        // Movie recording audio integration
        0xff8c3f9cL,  // Called by msg 4 handler (FUN_ff85d6cc) — MJPEG/audio state
        0xff8c4064L,  // Called by msg 4 handler — audio callback setup
        0xff8c4070L,  // Called by msg 4 handler — audio cleanup
    };

    private static final String[] NAMES = {
        "FUN_ff847334 — AudioTsk HW config (called 3x)",
        "task_AudioTsk — main audio task (re-decompile)",

        "FUN_ff846624 — AudioIC vicinity (init?)",
        "FUN_ff846764 — AudioIC vicinity (DMA?)",
        "FUN_ff8467a8 — AudioIC vicinity (cleanup?)",

        "FUN_ff84c170 — resource lookup (StartSoundRecord)",
        "FUN_ff84c144 — resource register (StartSoundRecord)",
        "FUN_ff84c118 — resource release (StartSoundRecord)",

        "FUN_ff846200 — AudioIC area",
        "FUN_ff846300 — AudioIC area",
        "FUN_ff846400 — AudioIC area",
        "FUN_ff846500 — AudioIC area",

        "FUN_ff8c3f9c — MJPEG/audio state (msg 4)",
        "FUN_ff8c4064 — audio callback (msg 4)",
        "FUN_ff8c4070 — audio cleanup (msg 4)",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\audio_hardware_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("AudioIC WM1400 Hardware Layer — Decompiled Functions\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("Purpose: Find live PCM DMA buffer address during movie recording\n");
        output.append("========================================================================\n\n");

        // First, read key ROM data values for AudioIC state struct
        output.append("--- Key ROM Data Values ---\n");
        long[] dataAddrs = {
            0xff846854L, 0xff846850L, 0xff846838L,
            0xff846858L, 0xff84685cL,
            0xff84c2a0L,
        };
        String[] dataNames = {
            "AudioTsk state struct", "AudioIC init param", "AudioIC string addr",
            "AudioTsk config1", "AudioTsk config2",
            "AudioIC hw state",
        };
        for (int i = 0; i < dataAddrs.length; i++) {
            Address addr = toAddr(dataAddrs[i]);
            try {
                int val = mem.getInt(addr);
                output.append(String.format("  0x%08X (%s) = 0x%08X\n",
                    dataAddrs[i], dataNames[i], val & 0xFFFFFFFFL));
            } catch (Exception e) {
                output.append(String.format("  0x%08X (%s) = [READ ERROR]\n",
                    dataAddrs[i], dataNames[i]));
            }
        }
        output.append("\n");

        // Decompile each function
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

                // Decompile
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

        // Scan AudioIC I/O register references (0xC0XX0000 pattern) in the
        // firmware near the AudioIC code area (0xff846000-0xff847400)
        output.append("\n========================================================================\n");
        output.append("AudioIC I/O Register Scan (0xFF846000-0xFF847400)\n");
        output.append("========================================================================\n\n");

        Address scanStart = toAddr(0xff846000L);
        Address scanEnd = toAddr(0xff847400L);
        for (long a = 0xff846000L; a < 0xff847400L; a += 4) {
            Address addr = toAddr(a);
            try {
                int val = mem.getInt(addr);
                // Look for I/O register addresses (0xC0xxxxxx) or DMA buffer addresses
                if ((val & 0xFF000000) == 0xC0000000) {
                    output.append(String.format("  0x%08X: 0x%08X  (I/O register)\n", a, val & 0xFFFFFFFFL));
                }
                // Look for RAM addresses that could be DMA buffers
                if (val > 0x10000 && val < 0x04000000 && (val & 0x3) == 0) {
                    // Check if it's in a literal pool (not code)
                    Address prev = toAddr(a - 4);
                    int prevVal = mem.getInt(prev);
                    // Simple heuristic: if both this and surrounding values look like data
                    if ((prevVal & 0xFF000000) != 0xE0000000) { // not ARM instruction
                        // Skip — too many false positives
                    }
                }
            } catch (Exception e) {
                // skip
            }
        }

        // Write output
        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();

        println("Audio hardware decompilation written to: " + OUTPUT_FILE);
    }
}
