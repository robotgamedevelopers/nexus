// Command environment keeps the shim's environment policy current with the modules it links. It scans the Go source of
// every package the shim (src/tailscale/shim) links on each platform nexus builds it for (the standard library
// included, nexus's own not), for each read of the process environment: os.Getenv, os.LookupEnv, os.Environ,
// syscall.Getenv and every tailscale.com/envknob lookup. Every variable read by a literal name must be listed in
// src/tailscale/environment.txt with what the shim does about it, and every read by a name the source computes must be
// listed there by its place; a read that is not listed, or a line that names nothing the source reads, fails.
//
//	go run ./environment -check   (from src/tailscale) fails unless environment.txt and shim/environment.go are current
//	go run ./environment          rewrites shim/environment.go from environment.txt (it never edits environment.txt)
//	  -go <path>                  the go command that lists the packages (default: go on PATH)
//	  -tags <a,b,...>             the shim's build tags (cmake/tailscale.cmake passes the omissions of pins.cmake)
//
// environment.txt has one line per entry, fields separated by tabs (lines starting with # are comments):
//
//	<NAME>	refuse	<why>     nts_start refuses while it is set (to anything, even empty)
//	<NAME>	forced	<why>     the shim sets it itself at every start (TS_NO_LOGS_NO_SUPPORT)
//	<NAME>	allow	<why>     read, but it cannot change control, sign-in, logging, uploads, keys or debugging
//	@<package>/<file>:<func>	allow|refuse	<why>   a read whose name the source computes
package main

import (
	"bytes"
	"flag"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

var platforms = []struct{ goos, goarch string }{
	{"darwin", "arm64"}, {"darwin", "amd64"}, {"linux", "amd64"}, {"windows", "amd64"},
}

const ownModules = "github.com/robotgamedevelopers/nexus/"
const envknobPath = "tailscale.com/envknob"

// The envknob functions whose first argument is a variable's name.
var envknobLookups = map[string]bool{
	"Bool": true, "BoolDefaultTrue": true, "LookupBool": true, "OptBool": true, "LookupInt": true,
	"LookupIntSized": true, "LookupUintSized": true, "String": true, "RegisterBool": true, "RegisterString": true,
	"RegisterOptBool": true, "RegisterDuration": true, "RegisterInt": true, "Setenv": true,
}

var (
	goCommand = "go"
	buildTags = ""
)

// A read: a literal name, or (name "") one the source computes, at where.
type read struct {
	name, where string
}

type pkg struct {
	path, dir string
	files     []string
}

func packages(goos, goarch string) ([]pkg, error) {
	cmd := exec.Command(goCommand, "list", "-deps", "-tags="+buildTags, "-f",
		"{{.ImportPath}}\t{{.Dir}}\t{{join .GoFiles \" \"}} {{join .CgoFiles \" \"}}",
		"./shim")
	cmd.Env = append(os.Environ(), "GOOS="+goos, "GOARCH="+goarch, "CGO_ENABLED=1")
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("go list for %s/%s: %v\n%s", goos, goarch, err, stderr.String())
	}
	var list []pkg
	for _, line := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		f := strings.SplitN(line, "\t", 3)
		if len(f) != 3 || strings.HasPrefix(f[0], ownModules) {
			continue
		}
		list = append(list, pkg{path: f[0], dir: f[1], files: strings.Fields(f[2])})
	}
	return list, nil
}

// scanPackage finds the environment reads in the files of one package. A name given by a string constant of the
// package counts as a literal name.
func scanPackage(pk pkg, skip map[string]bool) ([]read, error) {
	fset := token.NewFileSet()
	var files []*ast.File
	var paths []string
	for _, name := range pk.files {
		file := filepath.Join(pk.dir, name)
		f, err := parser.ParseFile(fset, file, nil, parser.SkipObjectResolution)
		if err != nil {
			return nil, err
		}
		files = append(files, f)
		paths = append(paths, file)
	}
	consts := map[string]string{}
	for _, f := range files {
		for _, decl := range f.Decls {
			g, ok := decl.(*ast.GenDecl)
			if !ok || g.Tok != token.CONST {
				continue
			}
			for _, spec := range g.Specs {
				v := spec.(*ast.ValueSpec)
				for i, id := range v.Names {
					if i < len(v.Values) {
						if lit, ok := v.Values[i].(*ast.BasicLit); ok && lit.Kind == token.STRING {
							if s, err := strconv.Unquote(lit.Value); err == nil {
								consts[id.Name] = s
							}
						}
					}
				}
			}
		}
	}
	var reads []read
	for i, f := range files {
		if skip[paths[i]] {
			continue
		}
		skip[paths[i]] = true
		reads = append(reads, scanFile(pk.path, paths[i], f, consts)...)
	}
	return reads, nil
}

// scanFile finds the environment reads in one parsed file of package path.
func scanFile(path, file string, f *ast.File, consts map[string]string) []read {
	names := map[string]string{} // local import name -> import path
	for _, imp := range f.Imports {
		p, _ := strconv.Unquote(imp.Path.Value)
		local := p[strings.LastIndex(p, "/")+1:]
		if imp.Name != nil {
			local = imp.Name.Name
		}
		names[local] = p
	}
	var reads []read
	var fn string
	add := func(call *ast.CallExpr) {
		where := path + "/" + filepath.Base(file) + ":" + fn
		if len(call.Args) > 0 {
			switch arg := call.Args[0].(type) {
			case *ast.BasicLit:
				if arg.Kind == token.STRING {
					if name, err := strconv.Unquote(arg.Value); err == nil {
						reads = append(reads, read{name: name, where: where})
						return
					}
				}
			case *ast.Ident:
				if name, ok := consts[arg.Name]; ok {
					reads = append(reads, read{name: name, where: where})
					return
				}
			}
		}
		reads = append(reads, read{where: where})
	}
	for _, decl := range f.Decls {
		fn = "(package)"
		if d, ok := decl.(*ast.FuncDecl); ok {
			fn = d.Name.Name
		}
		ast.Inspect(decl, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			switch fun := call.Fun.(type) {
			case *ast.SelectorExpr:
				x, ok := fun.X.(*ast.Ident)
				if !ok {
					return true
				}
				switch names[x.Name] {
				case "os":
					if fun.Sel.Name == "Getenv" || fun.Sel.Name == "LookupEnv" || fun.Sel.Name == "Environ" {
						add(call)
					}
				case "syscall":
					if fun.Sel.Name == "Getenv" {
						add(call)
					}
				case envknobPath:
					if envknobLookups[fun.Sel.Name] {
						add(call)
					}
				}
			case *ast.Ident:
				// Inside envknob, its own lookups by literal name (UseWIPCode, CanSSHD, ...).
				if path == envknobPath && envknobLookups[fun.Name] {
					if len(call.Args) > 0 {
						if _, lit := call.Args[0].(*ast.BasicLit); lit {
							add(call)
						}
					}
				}
			}
			return true
		})
	}
	return reads
}

// scan gives the names read by literal and the places of the reads by computed name, over every platform.
func scan() (map[string][]string, map[string]bool, error) {
	byName := map[string][]string{}
	computed := map[string]bool{}
	seen := map[string]bool{}
	for _, p := range platforms {
		list, err := packages(p.goos, p.goarch)
		if err != nil {
			return nil, nil, err
		}
		for _, pk := range list {
			reads, err := scanPackage(pk, seen)
			if err != nil {
				return nil, nil, err
			}
			for _, r := range reads {
				if r.name == "" {
					computed[r.where] = true
				} else if !contains(byName[r.name], r.where) {
					byName[r.name] = append(byName[r.name], r.where)
				}
			}
		}
	}
	return byName, computed, nil
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

type entry struct {
	key, what, why string
}

func readList(file string) ([]entry, error) {
	data, err := os.ReadFile(file)
	if err != nil {
		return nil, err
	}
	var list []entry
	for i, line := range strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n") {
		if strings.TrimSpace(line) == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.SplitN(line, "\t", 3)
		if len(f) != 3 || f[2] == "" {
			return nil, fmt.Errorf("%s:%d: want <name or @place><TAB><refuse|forced|allow><TAB><why>", file, i+1)
		}
		switch f[1] {
		case "refuse", "allow":
		case "forced":
			if strings.HasPrefix(f[0], "@") {
				return nil, fmt.Errorf("%s:%d: a computed read cannot be forced", file, i+1)
			}
		default:
			return nil, fmt.Errorf("%s:%d: %q is not refuse, forced or allow", file, i+1, f[1])
		}
		list = append(list, entry{f[0], f[1], f[2]})
	}
	return list, nil
}

// generated is shim/environment.go for the list.
func generated(list []entry) string {
	var refused, forced []string
	for _, e := range list {
		if strings.HasPrefix(e.key, "@") {
			continue
		}
		switch e.what {
		case "refuse":
			refused = append(refused, e.key)
		case "forced":
			forced = append(forced, e.key)
		}
	}
	sort.Strings(refused)
	sort.Strings(forced)
	var b strings.Builder
	b.WriteString("// Code generated by `go run ./environment` from src/tailscale/environment.txt; DO NOT EDIT.\n\n")
	b.WriteString("package main\n\n")
	b.WriteString("// refusedEnvironment: the variables the linked modules read that could change the control server, sign-in,\n")
	b.WriteString("// logging, uploads, TLS keys or debugging; nts_start refuses while any is set.\n")
	b.WriteString("var refusedEnvironment = []string{\n")
	for _, n := range refused {
		fmt.Fprintf(&b, "\t%q,\n", n)
	}
	b.WriteString("}\n\n")
	b.WriteString("// forcedEnvironment: the variables the shim sets itself at every start.\n")
	b.WriteString("var forcedEnvironment = []string{\n")
	for _, n := range forced {
		fmt.Fprintf(&b, "\t%q,\n", n)
	}
	b.WriteString("}\n")
	return b.String()
}

func main() {
	check := flag.Bool("check", false, "fail unless environment.txt covers the source and shim/environment.go is current")
	flag.StringVar(&goCommand, "go", "go", "the go command to list packages with")
	flag.StringVar(&buildTags, "tags", "", "the build tags the shim is built with, comma-separated")
	flag.Parse()
	list, err := readList("environment.txt")
	if err != nil {
		fmt.Fprintln(os.Stderr, "environment:", err)
		os.Exit(1)
	}
	text := generated(list)
	if !*check {
		if err := os.WriteFile(filepath.Join("shim", "environment.go"), []byte(text), 0o644); err != nil {
			fmt.Fprintln(os.Stderr, "environment:", err)
			os.Exit(1)
		}
		return
	}
	byName, computed, err := scan()
	if err != nil {
		fmt.Fprintln(os.Stderr, "environment:", err)
		os.Exit(1)
	}
	listed := map[string]bool{}
	var problems []string
	for _, e := range list {
		listed[e.key] = true
		if strings.HasPrefix(e.key, "@") {
			if !computed[e.key[1:]] {
				problems = append(problems, "listed but read nowhere now: "+e.key)
			}
		} else if byName[e.key] == nil && e.what != "forced" {
			problems = append(problems, "listed but read nowhere now: "+e.key)
		}
	}
	var names []string
	for name := range byName {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		if !listed[name] {
			problems = append(problems, "read but not listed: "+name+" (at "+strings.Join(byName[name], ", ")+")")
		}
	}
	var places []string
	for place := range computed {
		places = append(places, place)
	}
	sort.Strings(places)
	for _, place := range places {
		if !listed["@"+place] {
			problems = append(problems, "read by a computed name but not listed: @"+place)
		}
	}
	have, err := os.ReadFile(filepath.Join("shim", "environment.go"))
	if err != nil || strings.ReplaceAll(string(have), "\r\n", "\n") != text {
		problems = append(problems, "shim/environment.go is not what `go run ./environment` writes from environment.txt")
	}
	if len(problems) > 0 {
		fmt.Fprintln(os.Stderr, "environment: src/tailscale/environment.txt does not match the source the shim links:")
		for _, p := range problems {
			fmt.Fprintln(os.Stderr, "  "+p)
		}
		os.Exit(1)
	}
}
