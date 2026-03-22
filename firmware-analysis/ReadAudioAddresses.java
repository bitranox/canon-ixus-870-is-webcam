// ReadAudioAddresses.java — Ghidra headless script
// Reads ROM data values to find the RAM addresses of audio-related structures.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class ReadAudioAddresses extends GhidraScript {

    // ROM addresses containing pointers to RAM structs
    private static final long[] ROM_ADDRESSES = {
        // task_MovWrite audio handler references
        0xff92ec0cL,  // DAT_ff92ec0c — ring buffer struct used by audio metadata handler (case 4)
        0xff93050cL,  // DAT_ff93050c — ring buffer struct used by video/other handlers

        // Sound recording state struct
        0xffa09a28L,  // DAT_ffa09a28 — sound recording state struct pointer
        0xffa09a2cL,  // DAT_ffa09a2c — sample size multiplier

        // Audio codec / task references
        0xff846854L,  // DAT_ff846854 — AudioTsk state struct
        0xff846850L,  // DAT_ff846850 — AudioIC init param
        0xff846858L,  // DAT_ff846858 — AudioTsk config 1
        0xff84685cL,  // DAT_ff84685c — AudioTsk config 2

        // Sound recorder state
        0xff93856cL,  // DAT_ff93856c — SoundRecord state struct
        0xff939150L,  // DAT_ff939150 — SoundPlay state struct
        0xff93a2b4L,  // DAT_ff93a2b4 — WavWrite state struct
        0xff93a2b8L,  // DAT_ff93a2b8 — WavWrite buffer index array

        // Movie record task references
        0xff85d02cL,  // DAT_ff85d02c — movie_record_task state (iVar1 in msg 4 handler)
        0xff85d028L,  // DAT_ff85d028 — movie codec config
        0xff85d6a4L,  // DAT_ff85d6a4 — audio buffer base pointer in msg 4

        // Audio IC hardware config
        0xff84c2a0L,  // DAT_ff84c2a0 — AudioIC hardware state

        // task_MovWrite ring buffer offsets of interest
        // (read relative to the struct pointer)
    };

    private static final String[] NAMES = {
        "DAT_ff92ec0c — ring buf struct (audio case 4)",
        "DAT_ff93050c — ring buf struct (video/main)",

        "DAT_ffa09a28 — sound rec state struct",
        "DAT_ffa09a2c — sample size multiplier",

        "DAT_ff846854 — AudioTsk state",
        "DAT_ff846850 — AudioIC init param",
        "DAT_ff846858 — AudioTsk config 1",
        "DAT_ff84685c — AudioTsk config 2",

        "DAT_ff93856c — SoundRecord state",
        "DAT_ff939150 — SoundPlay state",
        "DAT_ff93a2b4 — WavWrite state",
        "DAT_ff93a2b8 — WavWrite buffer index",

        "DAT_ff85d02c — movie_record_task state",
        "DAT_ff85d028 — movie codec config",
        "DAT_ff85d6a4 — audio buf base (msg 4)",

        "DAT_ff84c2a0 — AudioIC hw state",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\audio_addresses.txt";

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("Audio Pipeline — ROM Data Values (RAM Address Pointers)\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        for (int i = 0; i < ROM_ADDRESSES.length; i++) {
            Address addr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(ROM_ADDRESSES[i]);

            output.append(String.format("%-50s  ROM 0x%08X = ", NAMES[i], ROM_ADDRESSES[i]));

            try {
                int val = mem.getInt(addr);
                output.append(String.format("0x%08X", val & 0xFFFFFFFFL));

                // If value looks like a RAM address, try to read what it points to
                if ((val & 0xFF000000) == 0x00000000 && val > 0x1000 && val < 0x04000000) {
                    output.append(String.format("  (RAM)"));
                }
            } catch (Exception e) {
                output.append("[READ ERROR: " + e.getMessage() + "]");
            }
            output.append("\n");
        }

        // Now read key offsets relative to the ring buffer structs
        output.append("\n\n========================================================================\n");
        output.append("Ring Buffer Struct Field Analysis\n");
        output.append("========================================================================\n\n");

        // Read DAT_ff92ec0c value first
        try {
            Address a1 = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(0xff92ec0cL);
            int ringBufAudio = mem.getInt(a1);

            Address a2 = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(0xff93050cL);
            int ringBufVideo = mem.getInt(a2);

            output.append(String.format("Audio ring buf struct (DAT_ff92ec0c) = 0x%08X\n", ringBufAudio & 0xFFFFFFFFL));
            output.append(String.format("Video ring buf struct (DAT_ff93050c) = 0x%08X\n", ringBufVideo & 0xFFFFFFFFL));

            if (ringBufAudio == ringBufVideo) {
                output.append(">>> SAME STRUCT — audio and video share the ring buffer!\n");
            } else {
                output.append(">>> DIFFERENT structs — audio and video use separate buffers\n");
            }

            output.append(String.format("\nAudio data offset within struct: +0x170 (108 bytes of PCM per frame)\n"));
            output.append(String.format("Audio metadata offset: +0x108 (8 bytes)\n"));
            output.append(String.format("If struct at 0x%08X:\n", ringBufAudio & 0xFFFFFFFFL));
            output.append(String.format("  Audio metadata addr: 0x%08X\n", (ringBufAudio + 0x108) & 0xFFFFFFFFL));
            output.append(String.format("  Audio data addr:     0x%08X\n", (ringBufAudio + 0x170) & 0xFFFFFFFFL));

        } catch (Exception e) {
            output.append("[ERROR reading ring buffer addresses: " + e.getMessage() + "]\n");
        }

        // Read the audio buffer base from msg 4 handler
        output.append("\n\n========================================================================\n");
        output.append("Msg 4 Audio Buffer Analysis\n");
        output.append("========================================================================\n\n");

        try {
            Address a3 = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(0xff85d6a4L);
            int audioBufBase = mem.getInt(a3);
            output.append(String.format("DAT_ff85d6a4 (audio buf base ptr) = 0x%08X\n", audioBufBase & 0xFFFFFFFFL));
            output.append(String.format("  iVar9 = *DAT_ff85d6a4 (value at RAM 0x%08X, read at runtime)\n",
                audioBufBase & 0xFFFFFFFFL));
            output.append(String.format("  Audio DMA buffer at: iVar9 + 0x219040\n"));
            output.append(String.format("  Audio ring data at:  iVar9 + 0x100040\n"));
            output.append(String.format("  Audio ring header:   iVar9 + 0x7C0\n"));
            output.append(String.format("  Audio index data:    iVar9 + 0x40\n"));
        } catch (Exception e) {
            output.append("[ERROR: " + e.getMessage() + "]\n");
        }

        // Read string references for audio-related strings
        output.append("\n\n========================================================================\n");
        output.append("Audio String References in ROM\n");
        output.append("========================================================================\n\n");

        long[] strAddrs = {
            0xff84683cL,  // "AudioIC_WM1400_c"
            0xff938570L,  // "SoundRecorder_c"
            0xff939154L,  // "SoundPlayer_c"
            0xff93a2bcL,  // "WavWriter_c"
            0xffa09a30L,  // "SoundDataSize"
            0xffa09a40L,  // "RECORDSOUNDDATA"
        };
        String[] strNames = {
            "AudioIC module",
            "SoundRecorder module",
            "SoundPlayer module",
            "WavWriter module",
            "Sound data size debug",
            "Record sound data resource",
        };

        for (int i = 0; i < strAddrs.length; i++) {
            Address addr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(strAddrs[i]);
            try {
                byte[] bytes = new byte[40];
                mem.getBytes(addr, bytes);
                StringBuilder str = new StringBuilder();
                for (byte b : bytes) {
                    if (b == 0) break;
                    str.append((char) b);
                }
                output.append(String.format("0x%08X: \"%s\" (%s)\n", strAddrs[i], str.toString(), strNames[i]));
            } catch (Exception e) {
                output.append(String.format("0x%08X: [READ ERROR] (%s)\n", strAddrs[i], strNames[i]));
            }
        }

        // Write output
        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();

        println("Audio addresses written to: " + OUTPUT_FILE);
    }
}
