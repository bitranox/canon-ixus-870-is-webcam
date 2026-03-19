// Read data values from ROM addresses
// @category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class ReadDataValues extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        long[] addrs = {
            0xFF85D02CL,  // DAT_ff85d02c — movie rec state struct ptr
            0xFF93050CL,  // DAT_ff93050c — ring buffer base ptr
            0xFF92EC0CL,  // DAT_ff92ec0c — ring buffer base ptr (alt)
            0xFF9304E0L,  // DAT_ff9304e0 — msg queue buffer base
        };
        for (int i = 0; i < addrs.length; i++) {
            Address addr = toAddr(addrs[i]);
            try {
                int val = mem.getInt(addr);
                println(String.format("DAT_%08x = 0x%08X (%d)", addrs[i], val, val));
            } catch (Exception e) {
                println(String.format("DAT_%08x = UNREADABLE: %s", addrs[i], e.getMessage()));
            }
        }
    }
}
