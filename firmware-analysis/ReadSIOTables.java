// ReadSIOTables.java — read ROM tables for SSIO DMA buffer register mapping

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class ReadSIOTables extends GhidraScript {

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\ssio_tables.txt";

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("SSIO DMA Register Pointer Tables\n\n");

        // Table 1: Register pointer pairs at 0xFFAE14D0 (DAT_ff8c0f6c)
        // Each entry is 8 bytes: (register_address, shift/param)
        output.append("=== Register Pointer Table (0xFFAE14D0) ===\n");
        output.append("Used by: *(int *)*puVar10 = piVar11[4]\n\n");
        for (int i = 0; i < 16; i++) {
            long addr = 0xFFAE14D0L + i * 4;
            try {
                int val = mem.getInt(toAddr(addr));
                output.append(String.format("  [%2d] 0x%08X = 0x%08X", i, addr, val & 0xFFFFFFFFL));
                if ((val & 0xFF000000) == 0xC0000000) output.append(" (I/O REG!)");
                else if (val > 0xFF800000L) output.append(" (ROM)");
                else if (val > 0x1000 && val < 0x04000000) output.append(" (RAM)");
                output.append("\n");
            } catch (Exception e) {
                output.append(String.format("  [%2d] 0x%08X = [ERROR]\n", i, addr));
            }
        }

        // Table 2: Channel config at 0xFFAE1570 (DAT_ff8c0f2c)
        // Each channel entry is 0x14 (20) bytes
        output.append("\n=== Channel Config Table (0xFFAE1570) ===\n");
        output.append("Each entry: 20 bytes (5 words)\n\n");
        for (int ch = 0; ch < 4; ch++) {
            output.append(String.format("Channel %d:\n", ch));
            for (int w = 0; w < 5; w++) {
                long addr = 0xFFAE1570L + ch * 0x14 + w * 4;
                try {
                    int val = mem.getInt(toAddr(addr));
                    output.append(String.format("  +0x%02X = 0x%08X", w * 4, val & 0xFFFFFFFFL));
                    if ((val & 0xFF000000) == 0xC0000000) output.append(" (I/O REG!)");
                    else if (val > 0xFF800000L) output.append(" (ROM)");
                    else if (val > 0x1000 && val < 0x04000000) output.append(" (RAM)");
                    output.append("\n");
                } catch (Exception e) {}
            }
        }

        // Table 3: ROM values referenced by SSIO driver
        output.append("\n=== SSIO Config ROM Values ===\n");
        long[] addrs = {0xFFAE14C8L, 0xFFAE14CCL};
        String[] names = {"DAT_ff8c0f5c (S540 config)", "DAT_ff8c0f60 (S544 config)"};
        for (int i = 0; i < addrs.length; i++) {
            try {
                int val = mem.getInt(toAddr(addrs[i]));
                output.append(String.format("  %s: 0x%08X\n", names[i], val & 0xFFFFFFFFL));
            } catch (Exception e) {}
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("SSIO tables written to: " + OUTPUT_FILE);
    }
}
