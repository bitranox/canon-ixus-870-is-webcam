// Ghidra headless script to decompile the msg 5 handler in movie_record_task.
//
// sub_FF85D3BC is the msg 5 handler that encodes the first H.264 frame (IDR)
// and writes the MOV container header. This is a critical function for
// understanding the movie recording startup sequence.
//
// We decompile the main function and any sub-functions it calls to get
// a complete picture of the IDR frame encoding and MOV header writing.
//
//@category CHDK
//@author webcam-re

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;
import java.io.FileWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileMsg5Handler extends GhidraScript {

    private static final long[][] TARGETS = {
        // {address, forceCreate}
        {0xFF85D3BCL, 1}, // msg 5 handler — IDR frame encode + MOV header write
    };

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager funcMgr = currentProgram.getFunctionManager();

        StringBuilder output = new StringBuilder();
        output.append("========================================================================\n");
        output.append("Msg 5 Handler: sub_FF85D3BC\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("Purpose: IDR frame encoding + MOV container header writing\n");
        output.append("========================================================================\n\n");

        Set<Long> decompiled = new LinkedHashSet<>();

        for (long[] target : TARGETS) {
            long addr = target[0];
            boolean forceCreate = target[1] == 1;

            Address funcAddr = toAddr(addr);
            Function func = funcMgr.getFunctionAt(funcAddr);

            if (func == null) {
                func = funcMgr.getFunctionContaining(funcAddr);
            }

            if (func == null && forceCreate) {
                output.append("// Creating function at 0x" +
                    Long.toHexString(addr).toUpperCase() + "...\n");
                println("Creating function at " + funcAddr + "...");
                try {
                    func = createFunction(funcAddr, "FUN_" +
                        Long.toHexString(addr).toLowerCase());
                    if (func != null) {
                        output.append("// Successfully created function: " +
                            func.getName() + " size=" + func.getBody().getNumAddresses() + "\n");
                    }
                } catch (Exception e) {
                    output.append("// ERROR creating function: " + e.getMessage() + "\n");
                }
            }

            if (func == null) {
                output.append("// No function at 0x" +
                    Long.toHexString(addr).toUpperCase() + "\n\n");
                continue;
            }

            long funcEntry = func.getEntryPoint().getOffset() & 0xFFFFFFFFL;
            if (decompiled.contains(funcEntry)) {
                output.append("// 0x" + Long.toHexString(addr).toUpperCase() +
                    " already decompiled (function at 0x" +
                    Long.toHexString(funcEntry).toUpperCase() + ")\n\n");
                continue;
            }
            decompiled.add(funcEntry);

            String name = func.getName();
            long bodySize = func.getBody().getNumAddresses();
            println("Decompiling " + name + " at " + func.getEntryPoint() +
                " (size=" + bodySize + " bytes)...");

            DecompileResults results = decomp.decompileFunction(func, 180, monitor);
            if (results == null || !results.decompileCompleted()) {
                output.append("// Decompilation FAILED for " + name +
                    " at " + func.getEntryPoint() + "\n");
                if (results != null) {
                    output.append("// Error: " + results.getErrorMessage() + "\n");
                }
                output.append("\n");
                continue;
            }

            DecompiledFunction decompFunc = results.getDecompiledFunction();
            if (decompFunc == null) {
                output.append("// No decompiled output for " + name +
                    " at " + func.getEntryPoint() + "\n\n");
                continue;
            }

            output.append("// === " + name + " @ " + func.getEntryPoint() +
                " (size=" + bodySize + " bytes) ===\n");
            output.append("// Signature: " + decompFunc.getSignature() + "\n\n");
            output.append(decompFunc.getC());
            output.append("\n\n\n");
        }

        String outputPath = "C:/projects/ixus870IS/firmware-analysis/msg5_handler_decompiled.txt";
        FileWriter writer = new FileWriter(outputPath);
        writer.write(output.toString());
        writer.close();

        println("\nDecompilation output written to: " + outputPath);
        println("\n" + output.toString());
    }
}
