// DecompileIDRArchitecture.java — Comprehensive decompilation of all IDR-related functions
// Run: analyzeHeadless <project> <name> -process -scriptPath . -postScript DecompileIDRArchitecture.java
//
// Targets: msg 4 internals, periodic IDR encoder, pipeline callbacks, ring buffer management,
// message posting (PostMessage/FUN_ff8279ec), callback at +0xA0, encoder setup functions

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.*;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class DecompileIDRArchitecture extends GhidraScript {

    // All functions we need to understand the IDR architecture
    private static final long[][] TARGETS = {
        // msg 4 internals (periodic IDR path)
        {0xFF85D6CC, 672},   // msg 4 handler (already have, but re-decompile for completeness)
        {0xFF8EE610, 0},     // Periodic IDR encoder (called by msg 4 with DMA+0x100040)
        {0xFF8EE558, 0},     // Encoder setup (called by msg 4)
        {0xFF8EE2E8, 0},     // Encode function (called by msg 4)
        {0xFF8EE800, 0},     // Encoder setup 2 (called by msg 4)
        {0xFF8EE4EC, 0},     // Encoder cleanup (called by msg 4)
        {0xFF8EE568, 0},     // Encoder intermediate (called by msg 4)
        {0xFF8EE7C8, 0},     // Encoder cleanup 2 (called by msg 4)
        {0xFF8EE810, 0},     // Encoder final (called by msg 4)

        // msg 5 encoder (first IDR)
        {0xFF8EDDFC, 560},   // FUN_ff8eddfc — JPCORE H.264 encoder (first frame)
        {0xFF8EE08C, 0},     // Setup before encode (called by msg 5)
        {0xFF8EE09C, 0},     // Cleanup after encode (called by msg 5)
        {0xFF8EE02C, 0},     // Another cleanup (called by msg 5)

        // Recording callbacks (fire for each pipeline frame)
        {0xFF85D370, 0},     // RecordingCallback1 (set by RecPipelineSetup)
        {0xFF85D28C, 0},     // RecordingCallback2 (set by RecPipelineSetup)
        {0xFF85DDA8, 0},     // Callback at +0xA0 (set by msg 2, called from sub_FF85D98C)
        {0xFF85DD14, 0},     // Callback at +0xA0 (set by msg 11 init)
        {0xFF85DDDC, 0},     // Callback for encoder setup (msg 2)
        {0xFF85DDE8, 0},     // Callback for ring buffer (msg 2)
        {0xFF85D670, 0},     // Callback in msg 4
        {0xFF85D664, 0},     // Callback in msg 4
        {0xFF85D6C0, 0},     // Callback in msg 4 (stop callback)
        {0xFF85D3B0, 0},     // Callback in msg 5

        // Ring buffer management
        {0xFF92FE8C, 552},   // MovieFrameGetter (msg 6 frame reader)
        {0xFF92FDF0, 156},   // msg 8 handler (frame committed)
        {0xFF92FD78, 0},     // IDR frame processing (called from msg 8)
        {0xFF92FC4C, 0},     // Special first-frame handling (called from MovieFrameGetter)
        {0xFF92FCD8, 0},     // Called from msg 2 recording start
        {0xFF92FB50, 0},     // Called from msg 7 stop
        {0xFF92F484, 0},     // Ring buffer init (called from msg 2)
        {0xFF92F734, 0},     // Ring buffer config (called from msg 2)
        {0xFF92F88C, 0},     // DMA cache writeback (called from msg 8)
        {0xFF930A68, 0},     // Frame validation (called from MovieFrameGetter)
        {0xFF9306B4, 0},     // Frame list management (called from MovieFrameGetter)
        {0xFF930358, 0},     // Called from sub_FF85D98C error paths
        {0xFF930390, 0},     // Called from sub_FF85D98C
        {0xFF9300B4, 0},     // Called from sub_FF85D98C
        {0xFF930EC0, 0},     // Called from msg 4

        // Message posting (PostMessage variants)
        {0xFF8279EC, 0},     // PostMessage / TryPostMessage
        {0xFF827098, 0},     // ReceiveMessage
        {0xFF85E260, 0},     // Message struct allocator (called before posting msg 5)

        // Pipeline setup
        {0xFF8C3BFC, 20},    // RecPipelineSetup
        {0xFF8C3D38, 84},    // StartMjpegMaking_Inner
        {0xFF8C3C94, 0},     // StopMjpegMaking_Inner
        {0xFF8C3F9C, 0},     // Pipeline intermediate
        {0xFF8C3FB4, 0},     // Pipeline cleanup (called by msg 5)
        {0xFF8C4064, 0},     // Stop callback setup (called by msg 4)
        {0xFF8C4070, 0},     // Resume after stop (called by msg 4)

        // Encoder setup / JPCORE control
        {0xFF8EDCE8, 0},     // Encoder registration (called from msg 2)
        {0xFF8EDC88, 0},     // Encoder control (called from sub_FF85D98C)
        {0xFF8EDCC4, 0},     // Encoder control 2
        {0xFF8EDA0C, 0},     // Encoder mode select (called from msg 2)

        // JPCORE register configuration
        {0xFF8EF7F8, 0},     // JPCORE_SetOutputBuf
        {0xFF8EFA6C, 0},     // PipelineRouting
        {0xFF8EFA80, 0},     // Pipeline param set
        {0xFF8EFABC, 0},     // JPCORE_RegisterCallback
        {0xFF8EF950, 0},     // JPCORE control
        {0xFF8EFD74, 0},     // JPCORE mode
        {0xFF8EFA44, 0},     // JPCORE param

        // Higher-level functions
        {0xFF8F2558, 0},     // Mode flag setter (0x81 vs 0x01 — IDR vs P-frame?)
        {0xFF8F2438, 0},     // Mode setup
        {0xFF8F211C, 0},     // Called during encode
        {0xFF8F2128, 0},     // Called during encode
        {0xFF8F14B8, 0},     // Called during encode
        {0xFF8F1654, 0},     // Callback registration

        // H.264 specific
        {0xFF92E324, 0},     // Called from msg 2 (quality setup?)
        {0xFF92E3B0, 0},     // Called from sub_FF85D98C (set_quality adjacent)
        {0xFF92BF38, 0},     // Called from PipelineFrameCallback

        // FFA0xxxx functions (H.264 encoder core?)
        {0xFFA0673C, 0},     // Called from FUN_ff8eddfc
        {0xFFA06798, 0},     // Called from FUN_ff8eddfc
        {0xFFA02B20, 0},     // Called from FUN_ff8eddfc
        {0xFFA0461C, 0},     // Called from FUN_ff8eddfc
        {0xFFA03F0C, 0},     // Called from FUN_ff8eddfc
        {0xFFA04170, 0},     // Called from FUN_ff8eddfc
        {0xFFA046DC, 0},     // Called from FUN_ff8eddfc
        {0xFFA04B80, 0},     // Called from FUN_ff8eddfc
        {0xFFA04B0C, 0},     // Called from FUN_ff8eddfc
        {0xFFA04B50, 0},     // Called from FUN_ff8eddfc
        {0xFFA048F0, 0},     // Called from FUN_ff8eddfc
        {0xFFA19C98, 0},     // Called from ring buffer functions
    };

    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs().length > 0 ? getScriptArgs()[0]
            : getSourceFile().getParentFile().getAbsolutePath() + "/idr_architecture_decompiled.txt";

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        decomp.setSimplificationStyle("decompile");

        FunctionManager funcMgr = currentProgram.getFunctionManager();
        PrintWriter out = new PrintWriter(new FileWriter(outPath));

        out.println("=======================================================================");
        out.println("IDR Architecture — Comprehensive Decompilation");
        out.println("Firmware: Canon IXUS 870 IS / SD880 IS, version 1.01a");
        out.println("Generated by DecompileIDRArchitecture.java (Ghidra headless)");
        out.println("Target: All functions in the IDR encoding, pipeline, and message paths");
        out.println("=======================================================================");
        out.println();

        int success = 0, fail = 0;

        for (long[] target : TARGETS) {
            long addr = target[0];
            Address funcAddr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addr);
            Function func = funcMgr.getFunctionAt(funcAddr);

            if (func == null) {
                // Try to create function at this address
                func = funcMgr.getFunctionAt(funcAddr);
                if (func == null) {
                    out.println("-----------------------------------------------------------------------");
                    out.printf("MISSING: No function at 0x%08X%n", addr);
                    out.println("-----------------------------------------------------------------------");
                    out.println();
                    fail++;
                    continue;
                }
            }

            out.println("-----------------------------------------------------------------------");
            out.printf("Function: %s @ 0x%08X (size=%d bytes)%n",
                func.getName(), addr, func.getBody().getNumAddresses());
            out.println("-----------------------------------------------------------------------");

            // Get cross-references TO this function
            ReferenceIterator refIter = currentProgram.getReferenceManager().getReferencesTo(funcAddr);
            List<Reference> refs = new ArrayList<>();
            while (refIter.hasNext()) refs.add(refIter.next());
            if (!refs.isEmpty()) {
                out.println("// Called from:");
                int shown = 0;
                for (Reference ref : refs) {
                    if (shown >= 10) { out.printf("//   ... and %d more%n", refs.size() - shown); break; }
                    Function caller = funcMgr.getFunctionContaining(ref.getFromAddress());
                    String callerName = caller != null ? caller.getName() : "unknown";
                    out.printf("//   0x%s (%s)%n", ref.getFromAddress(), callerName);
                    shown++;
                }
                out.println();
            }

            // Get cross-references FROM this function (calls it makes)
            Set<String> callees = new TreeSet<>();
            ghidra.program.model.listing.InstructionIterator instIter =
                currentProgram.getListing().getInstructions(func.getBody(), true);
            while (instIter.hasNext()) {
                ghidra.program.model.listing.Instruction inst = instIter.next();
                String mnemonic = inst.getMnemonicString();
                if (mnemonic.equals("BL") || mnemonic.equals("BLX")) {
                    Reference[] callRefs = inst.getReferencesFrom();
                    for (Reference r : callRefs) {
                        if (r.getReferenceType().isCall()) {
                            Function callee = funcMgr.getFunctionAt(r.getToAddress());
                            String calleeName = callee != null ? callee.getName() : "sub_" + r.getToAddress();
                            callees.add(String.format("0x%s (%s)", r.getToAddress(), calleeName));
                        }
                    }
                }
            }
            if (!callees.isEmpty()) {
                out.println("// Calls:");
                for (String c : callees) {
                    out.printf("//   %s%n", c);
                }
                out.println();
            }

            DecompileResults results = decomp.decompileFunction(func, 30, monitor);
            if (results.decompileCompleted()) {
                out.println(results.getDecompiledFunction().getC());
                success++;
            } else {
                out.printf("// DECOMPILATION FAILED: %s%n", results.getErrorMessage());
                fail++;
            }
            out.println();
            out.println();
        }

        // Also find ALL callers of FUN_ff8279ec (PostMessage) — who posts messages?
        out.println("=======================================================================");
        out.println("CROSS-REFERENCE: All callers of FUN_ff8279ec (PostMessage)");
        out.println("=======================================================================");
        Address postMsgAddr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0xFF8279EC);
        ReferenceIterator postMsgRefs = currentProgram.getReferenceManager().getReferencesTo(postMsgAddr);
        while (postMsgRefs.hasNext()) {
            Reference ref = postMsgRefs.next();
            Function caller = funcMgr.getFunctionContaining(ref.getFromAddress());
            String callerName = caller != null ? caller.getName() : "unknown";
            out.printf("  0x%s in %s%n", ref.getFromAddress(), callerName);
        }
        out.println();

        // Find all callers of the movie_record_task message queue receive (sub_FF827098)
        out.println("=======================================================================");
        out.println("CROSS-REFERENCE: All callers of FUN_ff827098 (ReceiveMessage)");
        out.println("=======================================================================");
        Address recvMsgAddr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0xFF827098);
        ReferenceIterator recvMsgRefs = currentProgram.getReferenceManager().getReferencesTo(recvMsgAddr);
        while (recvMsgRefs.hasNext()) {
            Reference ref = recvMsgRefs.next();
            Function caller = funcMgr.getFunctionContaining(ref.getFromAddress());
            String callerName = caller != null ? caller.getName() : "unknown";
            out.printf("  0x%s in %s%n", ref.getFromAddress(), callerName);
        }
        out.println();

        out.printf("=== SUMMARY: %d functions decompiled, %d failed ===%n", success, fail);
        out.close();
        decomp.dispose();

        printf("Wrote %d functions to %s (%d failed)%n", success, outPath, fail);
    }
}
