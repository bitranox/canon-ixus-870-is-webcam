// DecompileQualityCaller.java - Find and decompile the function containing address 0xFF824AD8
// This is the sole caller of FUN_ff849408 (JPCORE quality setter)

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressFactory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class DecompileQualityCaller extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outputPath = "C:/projects/ixus870IS/firmware-analysis/quality_caller_decompiled.txt";

        DecompInterface decomp = new DecompInterface();
        DecompileOptions options = new DecompileOptions();
        decomp.setOptions(options);
        decomp.openProgram(currentProgram);

        FunctionManager funcMgr = currentProgram.getFunctionManager();
        ReferenceManager refMgr = currentProgram.getReferenceManager();
        AddressFactory af = currentProgram.getAddressFactory();

        PrintWriter out = new PrintWriter(new FileWriter(outputPath));

        out.println("==========================================================================");
        out.println("JPCORE Quality Setter Caller Analysis");
        out.println("Goal: Find what function contains BL at 0xFF824AD8 -> FUN_ff849408");
        out.println("==========================================================================");
        out.println();

        // Step 1: Find the function CONTAINING address 0xFF824AD8
        long callerAddr = 0xff824ad8L;
        Address addr = af.getDefaultAddressSpace().getAddress(callerAddr);
        Function callerFunc = funcMgr.getFunctionContaining(addr);

        if (callerFunc != null) {
            out.println("##########################################################################");
            out.printf("# Function containing BL at 0x%08x%n", callerAddr);
            out.println("##########################################################################");
            out.println();
            out.println("Function: " + callerFunc.getName());
            out.println("Entry: " + callerFunc.getEntryPoint());
            out.println("Size: " + callerFunc.getBody().getNumAddresses() + " bytes");
            out.println("Signature: " + callerFunc.getPrototypeString(true, false));
            out.println();

            // Callers of this function
            out.println("--- CALLERS OF THIS FUNCTION ---");
            ReferenceIterator refsTo = refMgr.getReferencesTo(callerFunc.getEntryPoint());
            int cnt = 0;
            while (refsTo.hasNext()) {
                Reference ref = refsTo.next();
                Address from = ref.getFromAddress();
                Function caller = funcMgr.getFunctionContaining(from);
                String cname = (caller != null) ? caller.getName() + " (" + caller.getEntryPoint() + ")" : "(unknown)";
                out.println("  " + ref.getReferenceType() + " from " + from + " in " + cname);
                cnt++;
            }
            out.println("  Total: " + cnt);
            out.println();

            // Callees from this function
            out.println("--- CALLEES FROM THIS FUNCTION ---");
            Set<String> callees = new LinkedHashSet<String>();
            InstructionIterator ii = currentProgram.getListing().getInstructions(callerFunc.getBody(), true);
            while (ii.hasNext()) {
                Instruction instr = ii.next();
                Reference[] refs = instr.getReferencesFrom();
                for (Reference ref : refs) {
                    if (ref.getReferenceType().isCall()) {
                        Address to = ref.getToAddress();
                        Function cf = funcMgr.getFunctionAt(to);
                        String cn = (cf != null) ? cf.getName() + " (" + cf.getEntryPoint() + ")" : "(unknown at " + to + ")";
                        callees.add("  " + instr.getAddress() + " -> " + to + " = " + cn);
                    }
                }
            }
            for (String c : callees) { out.println(c); }
            out.println("  Total: " + callees.size());
            out.println();

            // Decompile
            out.println("--- DECOMPILED CODE ---");
            DecompileResults res = decomp.decompileFunction(callerFunc, 120, monitor);
            if (res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("DECOMPILATION FAILED: " + res.getErrorMessage());
            }
            out.println();
        } else {
            out.println("ERROR: No function found containing address 0xFF824AD8");
            out.println("Trying getFunctionAt instead...");
            Function funcAt = funcMgr.getFunctionAt(addr);
            if (funcAt != null) {
                out.println("Found function AT address: " + funcAt.getName());
            } else {
                out.println("No function at this address either.");
            }
        }

        // Step 2: Also decompile FUN_ff849408 itself for completeness
        out.println();
        out.println("##########################################################################");
        out.println("# FUN_ff849408 - JPCORE Quality Setter (for reference)");
        out.println("##########################################################################");
        out.println();

        Address qualAddr = af.getDefaultAddressSpace().getAddress(0xff849408L);
        Function qualFunc = funcMgr.getFunctionAt(qualAddr);
        if (qualFunc == null) qualFunc = funcMgr.getFunctionContaining(qualAddr);

        if (qualFunc != null) {
            out.println("Function: " + qualFunc.getName());
            out.println("Entry: " + qualFunc.getEntryPoint());
            out.println("Signature: " + qualFunc.getPrototypeString(true, false));
            out.println();

            // All callers via Ghidra references
            out.println("--- ALL CALLERS (Ghidra cross-references) ---");
            ReferenceIterator refsTo = refMgr.getReferencesTo(qualFunc.getEntryPoint());
            int cnt = 0;
            while (refsTo.hasNext()) {
                Reference ref = refsTo.next();
                Address from = ref.getFromAddress();
                Function caller = funcMgr.getFunctionContaining(from);
                String cname = (caller != null) ? caller.getName() + " (" + caller.getEntryPoint() + ")" : "(unknown)";
                out.println("  " + ref.getReferenceType() + " from " + from + " in " + cname);
                cnt++;
            }
            out.println("  Total: " + cnt);
            out.println();

            out.println("--- DECOMPILED CODE ---");
            DecompileResults res = decomp.decompileFunction(qualFunc, 120, monitor);
            if (res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("DECOMPILATION FAILED: " + res.getErrorMessage());
            }
        }

        // Step 3: Also check FUN_ff8496a8 (JPCORE init that sets quality to -1)
        out.println();
        out.println("##########################################################################");
        out.println("# FUN_ff8496a8 - JPCORE Init (sets quality to -1)");
        out.println("##########################################################################");
        out.println();

        Address initAddr = af.getDefaultAddressSpace().getAddress(0xff8496a8L);
        Function initFunc = funcMgr.getFunctionAt(initAddr);
        if (initFunc == null) initFunc = funcMgr.getFunctionContaining(initAddr);

        if (initFunc != null) {
            out.println("Function: " + initFunc.getName());
            out.println("Entry: " + initFunc.getEntryPoint());
            out.println();

            out.println("--- ALL CALLERS ---");
            ReferenceIterator refsTo = refMgr.getReferencesTo(initFunc.getEntryPoint());
            int cnt = 0;
            while (refsTo.hasNext()) {
                Reference ref = refsTo.next();
                Address from = ref.getFromAddress();
                Function caller = funcMgr.getFunctionContaining(from);
                String cname = (caller != null) ? caller.getName() + " (" + caller.getEntryPoint() + ")" : "(unknown)";
                out.println("  " + ref.getReferenceType() + " from " + from + " in " + cname);
                cnt++;
            }
            out.println("  Total: " + cnt);
            out.println();

            out.println("--- DECOMPILED CODE ---");
            DecompileResults res = decomp.decompileFunction(initFunc, 120, monitor);
            if (res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("DECOMPILATION FAILED: " + res.getErrorMessage());
            }
        }

        out.println();
        out.println("=== END ===");
        out.flush();
        out.close();
        decomp.dispose();

        println("Done. Output: " + outputPath);
    }
}
