// List references to addresses. Args: <outfile> <addr hex>...
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class XRefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(Long.parseLong(args[i], 16));
                pw.println("== refs to " + a);
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    pw.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " in " +
                        (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                }
            }
        }
    }
}
