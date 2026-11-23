// Trazado estatico del arranque, sobre el proyecto ya analizado con ReFineImportNames.java.
//
//   1. Arbol de llamadas directas desde _start, en el orden de las instrucciones de
//      llamada dentro de cada funcion (aproxima el orden de ejecucion), hasta
//      MAX_DEPTH. Marca con [HLE] las llamadas a imports.
//   2. BFS sobre el grafo de llamadas directas desde _start. Para cada modulo pedido
//      (cellGcmSys, cellAudio, sys_fs, cellSysutil por defecto) lista las llamadas a sus imports
//      ordenadas por profundidad desde _start y direccion, con el camino completo.
//
// Solo sigue llamadas directas (bl a una direccion conocida). Las llamadas por puntero
// (bctrl a traves de un OPD: callbacks, hilos, tablas virtuales) no se ven: lo que
// arranca un hilo con sys_ppu_thread_create queda fuera del arbol.
//
//   analyzeHeadless work/ghidra ReFine -process EBOOT.elf -noanalysis -readOnly \
//       -scriptPath tools/ghidra -postScript ReFineTraceBoot.java [modulo ...] [@sitios.tsv]
//@category Re:Fine

import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;

public class ReFineTraceBoot extends GhidraScript {

    private static final int MAX_DEPTH = 4;
    private static final int MAX_SITES = 15;

    /** Llamada directa: desde que instruccion, a que funcion. */
    private record Call(Address site, Function callee) {}

    private final Map<Function, List<Call>> callCache = new HashMap<>();

    @Override
    protected void run() throws Exception {
        String[] mods = getScriptArgs();
        // @<tsv de tools/rpcs3_hle_seq.py>: ademas, ubicar cada llamada de RPCS3. (No "x=y":
        // analyzeHeadless.bat parte los argumentos por el "=".)
        String sitesFile = null;
        List<String> modList = new ArrayList<>();
        for (String a : mods) {
            if (a.startsWith("@")) {
                sitesFile = a.substring(1);
            } else {
                modList.add(a);
            }
        }
        mods = modList.toArray(new String[0]);
        if (mods.length == 0) {
            mods = new String[] { "cellGcmSys", "cellAudio", "sys_fs", "cellSysutil" };
        }

        Function start = getGlobalFunctions("_start").stream().findFirst().orElse(null);
        if (start == null) {
            throw new IllegalStateException("No hay _start: corre antes ReFineImportNames.java");
        }

        println("=== Arbol de llamadas directas desde _start " + start.getEntryPoint()
            + " (profundidad " + MAX_DEPTH + ") ===");
        printTree(start, 0, new HashSet<>());

        // BFS: profundidad minima y padre de cada funcion alcanzable.
        Map<Function, Integer> depth = new LinkedHashMap<>();
        Map<Function, Call> parentCall = new HashMap<>();
        Map<Function, Function> parent = new HashMap<>();
        ArrayDeque<Function> q = new ArrayDeque<>();
        depth.put(start, 0);
        q.add(start);
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
                    parentCall.put(c.callee(), c);
                    q.add(c.callee());
                }
            }
        }
        println("");
        println("funciones alcanzables por llamadas directas: " + depth.size());

        for (String mod : mods) {
            println("");
            println("=== Llamadas a " + mod + " (por profundidad desde _start) ===");
            List<Object[]> sites = new ArrayList<>();
            for (Map.Entry<Function, Integer> e : depth.entrySet()) {
                Function caller = e.getKey();
                if (isImport(caller)) {
                    continue;
                }
                for (Call c : calls(caller)) {
                    if (isImport(c.callee()) && mod.equals(c.callee().getParentNamespace().getName())) {
                        sites.add(new Object[] { e.getValue(), c.site(), caller, c.callee() });
                    }
                }
            }
            sites.sort(Comparator.comparingInt((Object[] s) -> (Integer) s[0])
                .thenComparing(s -> (Address) s[1]));
            if (sites.isEmpty()) {
                println("  (ninguna alcanzable por llamadas directas)");
            }
            for (int i = 0; i < Math.min(MAX_SITES, sites.size()); i++) {
                Object[] s = sites.get(i);
                Function caller = (Function) s[2];
                println(String.format("  %s  %-40s prof %d  en %s", s[1],
                    ((Function) s[3]).getName(), (Integer) s[0], name(caller)));
                println("      camino: " + path(caller, parent));
            }
            if (sites.size() > MAX_SITES) {
                println("  ... " + (sites.size() - MAX_SITES) + " mas");
            }
        }

        if (sitesFile != null) {
            locateSites(new java.io.File(sitesFile), depth, parent);
        }
    }

    /**
     * Para cada fila del TSV de rpcs3_hle_seq.py con origen EBOOT: funcion que contiene la
     * llamada (LR-4 para HLE, PC para syscalls) y si es alcanzable desde _start por
     * llamadas directas (profundidad) o no (se llega por puntero: hilo, callback, vtable).
     */
    private void locateSites(java.io.File tsv, Map<Function, Integer> depth,
            Map<Function, Function> parent) throws Exception {
        println("");
        println("=== Llamadas de RPCS3 ubicadas en el EBOOT (" + tsv.getName() + ") ===");
        println("n\tfuncion\tsitio\tcontenida_en\tprofundidad\tcamino");
        int reach = 0, total = 0;
        for (String line : java.nio.file.Files.readAllLines(tsv.toPath())) {
            String[] c = line.split("\t");
            if (c.length < 7 || c[0].equals("n") || !c[3].equals("EBOOT")) {
                continue;
            }
            String dir = c[4];
            long a = Long.parseLong(dir.substring(dir.indexOf("0x") + 2), 16);
            long site = dir.startsWith("LR:") ? a - 4 : a;
            Address s = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(site);
            Function f = getFunctionContaining(s);
            total++;
            String where = f == null ? "?" : name(f);
            String d = "NO";
            String p = "";
            if (f != null && depth.containsKey(f)) {
                d = String.valueOf(depth.get(f));
                p = path(f, parent);
                reach++;
            }
            println(String.join("\t", c[0], c[6], s.toString(), where, d, p));
        }
        println(String.format("alcanzables por llamadas directas: %d de %d", reach, total));
    }

    private void printTree(Function f, int level, Set<Function> onPath) throws Exception {
        if (level >= MAX_DEPTH) {
            return;
        }
        onPath.add(f);
        for (Call c : calls(f)) {
            monitor.checkCancelled();
            Function g = c.callee();
            String mark = isImport(g) ? " [HLE " + g.getParentNamespace().getName() + "]" : "";
            println(String.format("%s%s -> %s %s%s", "  ".repeat(level + 1), c.site(),
                g.getEntryPoint(), name(g), mark));
            if (!isImport(g) && !onPath.contains(g)) {
                printTree(g, level + 1, onPath);
            }
        }
        onPath.remove(f);
    }

    /** Llamadas directas de f, en orden de direccion de la instruccion. */
    private List<Call> calls(Function f) {
        List<Call> out = callCache.get(f);
        if (out != null) {
            return out;
        }
        out = new ArrayList<>();
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            FlowType ft = ins.getFlowType();
            if (!ft.isCall() || ft.isComputed()) {
                continue;
            }
            for (Reference r : ins.getReferencesFrom()) {
                if (!r.getReferenceType().isCall()) {
                    continue;
                }
                Function g = getFunctionAt(r.getToAddress());
                if (g != null) {
                    out.add(new Call(ins.getAddress(), g));
                }
            }
        }
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
