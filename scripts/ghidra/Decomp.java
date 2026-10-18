// Decompile functions to a text file. Args: <outfile> <addr hex>...  (address may be inside function)
// If no function contains the address, prefix the address with '!' to create one starting there.
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;

public class Decomp extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                boolean create = args[i].startsWith("!");
                String s = create ? args[i].substring(1) : args[i];
                Address a = toAddr(Long.parseLong(s, 16));
                Function f = getFunctionContaining(a);
                if (f == null && create) {
                    disassemble(a);
                    f = createFunction(a, null);
                }
                if (f == null) { pw.println("// no function at " + s); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                pw.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
                pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// FAILED: " + r.getErrorMessage());
            }
        }
    }
}
