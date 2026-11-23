// Mapa estatico del arranque de F, sobre el proyecto ya analizado con ReFineImportNames.java.
//
//   1. Diagnostico de una funcion raiz (por defecto, la que _start alcanza con mas llamadas):
//      cuerpo, numero de llamadas directas y a que.
//   2. BFS por llamadas directas desde la raiz dada (main) y, para cada modulo pedido, las
//      llamadas a sus imports con la profundidad y el camino desde la raiz.
//   3. Cada llamada a sysPrxForUser::sys_ppu_thread_create: entry (r4), prioridad (r6),
//      tamano de pila (r7) y nombre (r9) cuando son constantes en el mismo bloque
//      (li / lis+addi|ori / lwz rX,d(r2) leyendo la TOC / addi rX,r2,d / mr).
//   4. Inventario de syscalls directos (sc con li r11,N delante): numero, sitios y funcion.
//
// Solo sigue llamadas directas (bl). Lo que se llama por puntero (hilos, callbacks,
// vtables) queda fuera del BFS; para eso esta el log de RPCS3.
//
//   analyzeHeadless work/ghidra ReFine -process EBOOT.elf -noanalysis -readOnly \
//       -scriptPath tools/ghidra -postScript ReFineBootMap.java 0x3df8c [modulo ...]
//@category Re:Fine

import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;

public class ReFineBootMap extends GhidraScript {

    private static final int MAX_SITES = 25;
    private static final int BACK = 40;   // instrucciones hacia atras al buscar un registro

    private record Call(Address site, Function callee) {}

    private final Map<Function, List<Call>> callCache = new HashMap<>();
    private long toc;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            throw new IllegalArgumentException("uso: ReFineBootMap.java <raiz 0x...> [modulo ...]");
        }
        Address rootA = toAddr(Long.decode(args[0]));
        Function root = getFunctionAt(rootA);
        if (root == null) {
            throw new IllegalStateException("no hay funcion en " + rootA);
        }
        List<String> mods = new ArrayList<>(Arrays.asList(args).subList(1, args.length));
        if (mods.isEmpty()) {
            mods = List.of("cellGcmSys", "cellAudio", "sys_fs", "cellSysutil", "cellGame", "cellResc");
        }
        Register r2 = currentProgram.getRegister("r2");
        java.math.BigInteger t = currentProgram.getProgramContext().getValue(r2, rootA, false);
        toc = t == null ? 0 : t.longValue();
        println(String.format("r2 (TOC) en la raiz: 0x%x", toc));

        // 1. diagnostico de la raiz
        println("=== Raiz " + name(root) + " @ " + root.getEntryPoint() + " ===");
        println("cuerpo: " + root.getBody());
        List<Call> rc = calls(root);
        println("llamadas directas: " + rc.size());
        for (Call c : rc) {
            println(String.format("  %s -> %s %s", c.site(), c.callee().getEntryPoint(), name(c.callee())));
        }

        // 2. BFS desde la raiz
        Map<Function, Integer> depth = new LinkedHashMap<>();
        Map<Function, Function> parent = new HashMap<>();
        ArrayDeque<Function> q = new ArrayDeque<>();
        depth.put(root, 0);
        q.add(root);
        while (!q.isEmpty()) {
            monitor.checkCancelled();
            Function f = q.poll();
            if (isImport(f)) {
                continue;
            }
            for (Call c : calls(f)) {
                if (!depth.containsKey(c.callee())) {
                    depth.put(c.callee(), depth.get(f) + 1);
                    parent.put(c.callee(), f);
                    q.add(c.callee());
                }
            }
        }
        println("");
        println("funciones alcanzables desde la raiz por llamadas directas: " + depth.size());
        for (String mod : mods) {
            println("");
            println("=== Llamadas a " + mod + " desde la raiz (por profundidad) ===");
            List<Object[]> sites = new ArrayList<>();
            for (Map.Entry<Function, Integer> e : depth.entrySet()) {
                if (isImport(e.getKey())) {
                    continue;
                }
                for (Call c : calls(e.getKey())) {
                    if (isImport(c.callee()) && mod.equals(c.callee().getParentNamespace().getName())) {
                        sites.add(new Object[] { e.getValue(), c.site(), e.getKey(), c.callee() });
                    }
                }
            }
            sites.sort(Comparator.comparingInt((Object[] s) -> (Integer) s[0]).thenComparing(s -> (Address) s[1]));
            if (sites.isEmpty()) {
                println("  (ninguna alcanzable por llamadas directas)");
            }
            for (int i = 0; i < Math.min(MAX_SITES, sites.size()); i++) {
                Object[] s = sites.get(i);
                println(String.format("  %s  %-40s prof %d  en %s", s[1], ((Function) s[3]).getName(),
                    (Integer) s[0], name((Function) s[2])));
                println("      camino: " + path((Function) s[2], parent));
            }
            if (sites.size() > MAX_SITES) {
                println("  ... " + (sites.size() - MAX_SITES) + " mas");
            }
        }

        // 3. sys_ppu_thread_create
        println("");
        println("=== sysPrxForUser::sys_ppu_thread_create: sitios ===");
        println("sitio\tfuncion\tprof_desde_raiz\tentry_opd(r4)\tprio(r6)\tpila(r7)\tnombre(r9)");
        for (Function imp : getGlobalFunctionsInNs("sysPrxForUser", "sys_ppu_thread_create")) {
            for (Reference r : getReferencesTo(imp.getEntryPoint())) {
                if (!r.getReferenceType().isCall()) {
                    continue;
                }
                Address site = r.getFromAddress();
                Function f = getFunctionContaining(site);
                String d = f != null && depth.containsKey(f) ? String.valueOf(depth.get(f)) : "NO";
                Long r4 = regConst(site, "r4"), r6 = regConst(site, "r6"), r7 = regConst(site, "r7"), r9 = regConst(site, "r9");
                println(String.join("\t", site.toString(), f == null ? "?" : name(f), d,
                    hex(r4), r6 == null ? "?" : String.valueOf(r6), hex(r7), r9 == null ? "?" : quote(r9)));
            }
        }

        // 4. syscalls directos
        println("");
        println("=== syscalls directos (sc) ===");
        TreeMap<Long, List<Address>> sys = new TreeMap<>();
        int unknown = 0;
        for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
            if (!ins.getMnemonicString().equals("sc")) {
                continue;
            }
            Long n = regConst(ins.getAddress(), "r11");
            if (n == null) {
                unknown++;
                continue;
            }
            sys.computeIfAbsent(n, k -> new ArrayList<>()).add(ins.getAddress());
        }
        println("numero\tsitios\tejemplos (sitio en funcion)");
        for (Map.Entry<Long, List<Address>> e : sys.entrySet()) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < Math.min(4, e.getValue().size()); i++) {
                Address a = e.getValue().get(i);
                Function f = getFunctionContaining(a);
                sb.append(a).append(" en ").append(f == null ? "?" : name(f)).append("; ");
            }
            println(e.getKey() + "\t" + e.getValue().size() + "\t" + sb);
        }
        println("sc sin li r11 reconocible: " + unknown);
    }

    private List<Function> getGlobalFunctionsInNs(String ns, String fn) {
        List<Function> out = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName().equals(fn) && f.getParentNamespace().getName().equals(ns)) {
                out.add(f);
            }
        }
        return out;
    }

    /** Valor constante de un registro justo antes de la instruccion en 'at', o null. */
    private Long regConst(Address at, String reg) {
        Instruction ins = getInstructionAt(at);
        for (int i = 0; i < BACK && ins != null; i++) {
            ins = ins.getPrevious();
            if (ins == null) {
                return null;
            }
            FlowType ft = ins.getFlowType();
            if (ft.isCall() || ft.isTerminal()) {
                return null;   // una llamada pisa los volatiles; fin de bloque
            }
            Object[] o0 = ins.getOpObjects(0);
            if (o0.length != 1 || !(o0[0] instanceof Register) || !((Register) o0[0]).getName().equals(reg)) {
                continue;
            }
            String m = ins.getMnemonicString();
            switch (m) {
                case "li":
                    return scalar(ins, 1);
                case "lis": {
                    Long hi = scalar(ins, 1);
                    return hi == null ? null : (hi << 16) & 0xffffffffL;
                }
                case "addi": case "ori": case "lwz": case "mr": {
                    Object[] o1 = ins.getOpObjects(1);
                    String src = null;
                    Long imm = null;
                    for (Object x : o1) {
                        if (x instanceof Register) src = ((Register) x).getName();
                        if (x instanceof Scalar) imm = ((Scalar) x).getSignedValue();
                    }
                    if (imm == null) {
                        imm = scalar(ins, 2);
                        Object[] o2 = ins.getOpObjects(2);
                        for (Object x : o2) if (x instanceof Register) src = ((Register) x).getName();
                        if (src == null && o1.length == 1 && o1[0] instanceof Register) src = ((Register) o1[0]).getName();
                    }
                    if (m.equals("mr")) {
                        return src == null ? null : regConst(ins.getAddress(), src);
                    }
                    if (src == null || imm == null) return null;
                    Long base = src.equals("r2") ? tocAt(ins.getAddress()) : regConst(ins.getAddress(), src);
                    if (base == null) return null;
                    long ea = m.equals("ori") ? (base | (imm & 0xffff)) : (base + imm) & 0xffffffffL;
                    if (m.equals("lwz")) {
                        try {
                            return getInt(toAddr(ea)) & 0xffffffffL;
                        } catch (Exception e) {
                            return null;
                        }
                    }
                    return ea;
                }
                default:
                    return null;   // otra escritura del registro: no constante
            }
        }
        return null;
    }

    /** r2 en esa direccion (F tiene dos TOC: la del modulo y la de las librerias del SDK). */
    private Long tocAt(Address a) {
        java.math.BigInteger v = currentProgram.getProgramContext()
            .getValue(currentProgram.getRegister("r2"), a, false);
        return v == null ? Long.valueOf(toc) : Long.valueOf(v.longValue());
    }

    private Long scalar(Instruction ins, int op) {
        for (Object x : ins.getOpObjects(op)) {
            if (x instanceof Scalar) return ((Scalar) x).getSignedValue();
        }
        return null;
    }

    private String hex(Long v) {
        return v == null ? "?" : String.format("0x%x", v);
    }

    private String quote(long ea) {
        try {
            StringBuilder sb = new StringBuilder();
            Address a = toAddr(ea);
            for (int i = 0; i < 64; i++) {
                byte b = getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7e) return String.format("0x%x", ea);
                sb.append((char) b);
            }
            return String.format("\"%s\" (0x%x)", sb, ea);
        } catch (Exception e) {
            return String.format("0x%x", ea);
        }
    }

    /**
     * Llamadas directas de f siguiendo el flujo de codigo desde su entrada, no su cuerpo en
     * Ghidra: el compilador de F (SNC) pone a veces el cuerpo antes de la entrada, que es un
     * solo "b" hacia atras (main en 0x3df8c: "b 0x3def4"), y los rangos OPD lo cortan. Se
     * siguen saltos condicionales e incondicionales; un "b" a la entrada de otra funcion es
     * una llamada de cola; blr, bctr y bctrl/llamadas por puntero cierran ese camino.
     */
    private List<Call> calls(Function f) {
        List<Call> out = callCache.get(f);
        if (out != null) return out;
        out = new ArrayList<>();
        Set<Address> seen = new HashSet<>();
        ArrayDeque<Address> work = new ArrayDeque<>();
        work.add(f.getEntryPoint());
        while (!work.isEmpty() && seen.size() < 50000) {
            Address a = work.poll();
            while (a != null && seen.add(a)) {
                Instruction ins = getInstructionAt(a);
                if (ins == null) break;
                FlowType ft = ins.getFlowType();
                if (ft.isCall()) {
                    if (!ft.isComputed()) {
                        for (Address t : ins.getFlows()) {
                            Function g = getFunctionAt(t);
                            if (g != null) out.add(new Call(ins.getAddress(), g));
                        }
                    }
                    a = ins.getFallThrough();
                    continue;
                }
                if (ft.isJump()) {
                    if (ft.isComputed()) break;   // bctr: tabla de saltos o puntero, no se sigue
                    for (Address t : ins.getFlows()) {
                        Function g = getFunctionAt(t);
                        if (g != null && !g.equals(f)) {
                            out.add(new Call(ins.getAddress(), g));   // llamada de cola
                        } else {
                            work.add(t);
                        }
                    }
                    a = ft.isConditional() ? ins.getFallThrough() : null;
                    continue;
                }
                if (ft.isTerminal()) break;
                a = ins.getFallThrough();
            }
        }
        out.sort(Comparator.comparing(Call::site));
        callCache.put(f, out);
        return out;
    }

    private boolean isImport(Function f) {
        return !f.getParentNamespace().isGlobal();
    }

    private String name(Function f) {
        return isImport(f) ? f.getParentNamespace().getName() + "::" + f.getName() : f.getName();
    }

    private String path(Function f, Map<Function, Function> parent) {
        LinkedList<String> p = new LinkedList<>();
        for (Function x = f; x != null; x = parent.get(x)) {
            p.addFirst(name(x) + "@" + x.getEntryPoint());
        }
        return String.join(" > ", p);
    }
}
