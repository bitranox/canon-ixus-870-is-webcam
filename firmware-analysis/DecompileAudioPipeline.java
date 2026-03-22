// DecompileAudioPipeline.java — Ghidra headless script
// Decompiles the audio recording pipeline to find PCM buffer addresses.
//
// Key functions:
//   task_AudioTsk     (0xff8465cc) — main audio processing task
//   task_SoundRecord  (0xff938648) — recording state manager
//   task_SoundPlay    (0xff938fb4) — playback (for reference)
//   task_WavWrite     (0xff939fe0) — WAV file writer
//   task_WavRead      (0xff939608) — WAV file reader (for reference)
//   InitializeSoundRec (0xffa0981c) — sound recording init
//   StartSoundRecord  (0xffa0979c) — start recording
//   TerminateSoundRec (0xffa09850) — terminate recording
//   FreeBufferForSoundRec (0xffa09720) — free sound buffer (reveals buffer address)

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAudioPipeline extends GhidraScript {

    private static final long[] ADDRESSES = {
        0xff8465ccL,  // task_AudioTsk
        0xff938648L,  // task_SoundRecord
        0xff938fb4L,  // task_SoundPlay
        0xff939fe0L,  // task_WavWrite
        0xffa0981cL,  // InitializeSoundRec_FW
        0xffa0979cL,  // StartSoundRecord_FW
        0xffa09850L,  // TerminateSoundRec_FW
        0xffa09720L,  // FreeBufferForSoundRec_FW
    };

    private static final String[] NAMES = {
        "task_AudioTsk — main audio processing task",
        "task_SoundRecord — recording state manager",
        "task_SoundPlay — playback task",
        "task_WavWrite — WAV file writer",
        "InitializeSoundRec_FW — sound recording initialization",
        "StartSoundRecord_FW — start sound recording",
        "TerminateSoundRec_FW — terminate sound recording",
        "FreeBufferForSoundRec_FW — free sound recording buffer (reveals buffer addr)",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\audio_pipeline_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Audio Recording Pipeline — Decompiled Functions\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("Purpose: Trace PCM audio buffer addresses for webcam audio capture\n");
        output.append("========================================================================\n\n");

        for (int i = 0; i < ADDRESSES.length; i++) {
            Address addr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(ADDRESSES[i]);
            Function func = fm.getFunctionAt(addr);

            output.append("========================================================================\n");
            output.append(String.format("Function %d/%d: %s\n", i + 1, ADDRESSES.length, NAMES[i]));
            output.append(String.format("Address: 0x%08X\n", ADDRESSES[i]));
            output.append("========================================================================\n");

            if (func == null) {
                // Try to create function at address
                func = createFunction(addr, null);
            }

            if (func != null) {
                output.append("Function name in Ghidra: " + func.getName() + "\n");
                output.append("Function size: " + func.getBody().getNumAddresses() + " bytes\n");

                // Get all called functions for cross-reference
                Function[] calledFuncs = getCalledFunctions(func);
                if (calledFuncs.length > 0) {
                    output.append("\nCalls:\n");
                    for (Function cf : calledFuncs) {
                        output.append(String.format("  -> 0x%s (%s)\n",
                            cf.getEntryPoint().toString(), cf.getName()));
                    }
                }

                // Get callers
                Function[] callers = getCallingFunctions(func);
                if (callers.length > 0) {
                    output.append("\nCalled by:\n");
                    for (Function cf : callers) {
                        output.append(String.format("  <- 0x%s (%s)\n",
                            cf.getEntryPoint().toString(), cf.getName()));
                    }
                }

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

        // Now decompile any interesting functions called by the above
        // that might contain buffer addresses (look for malloc, alloc, DMA setup)
        output.append("\n\n========================================================================\n");
        output.append("END OF PRIMARY DECOMPILATION\n");
        output.append("========================================================================\n");

        // Write output
        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();

        println("Audio pipeline decompilation written to: " + OUTPUT_FILE);
    }

    private Function[] getCalledFunctions(Function func) {
        java.util.Set<Function> called = func.getCalledFunctions(monitor);
        return called.toArray(new Function[0]);
    }

    private Function[] getCallingFunctions(Function func) {
        java.util.Set<Function> callers = func.getCallingFunctions(monitor);
        return callers.toArray(new Function[0]);
    }
}
