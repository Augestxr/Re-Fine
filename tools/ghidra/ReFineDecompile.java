// Descompila las funciones que contienen las direcciones dadas (para diagnostico).
//
//   analyzeHeadless work/ghidra ReFine -process EBOOT.elf -noanalysis -readOnly \
//       -scriptPath tools/ghidra -postScript ReFineDecompile.java 0x2e5780 0x67edc4
//
// Imprime, por cada direccion, la funcion que la contiene, sus llamadores directos y
// el pseudo-C del decompilador de Ghidra. Solo lectura.
//@category Re:Fine

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class ReFineDecompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            long off = Long.decode(a);
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(off);
            Function f = getFunctionContaining(addr);
            println("==== " + a + " -> " + (f == null ? "(sin funcion)" : f.getName() + " @ " + f.getEntryPoint()));
            if (f == null) continue;
            StringBuilder callers = new StringBuilder();
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                if (r.getReferenceType().isCall()) callers.append(' ').append(r.getFromAddress());
            }
            println("llamada desde:" + callers);
            DecompileResults res = dec.decompileFunction(f, 120, monitor);
            println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "(fallo: " + res.getErrorMessage() + ")");
        }
        dec.dispose();
    }
}
