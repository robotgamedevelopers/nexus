// Command notices writes src/tailscale/NOTICES.md and src/tailscale/shim/licences.txt: every Go module the shim
// (src/tailscale/shim) links on each platform nexus builds it for, with the licence its licence files state and the
// verbatim licence and notice texts embedded into the shim, measured by `go list -deps` per platform. It fails on a
// module whose licence it cannot name, or one nexus (AGPL-3.0) cannot combine with.
//
//	go run ./notices          (from src/tailscale) rewrites NOTICES.md and shim/licences.txt
//	go run ./notices -check   fails if NOTICES.md or shim/licences.txt is not what it would write
//	  -go <path>              the go command that lists the modules (default: go on PATH)
//	  -tags <a,b,...>         the shim's build tags (cmake/tailscale.cmake passes the omissions of pins.cmake)
//
// Run it with the pinned toolchain (GOTOOLCHAIN=go1.27.4, as cmake/tailscale.cmake does) after any change to go.mod.
package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
)

// The platforms cmake/tailscale.cmake builds the shim for.
var platforms = []struct{ goos, goarch string }{
	{"darwin", "arm64"}, {"darwin", "amd64"}, {"linux", "amd64"}, {"windows", "amd64"},
}

// Licences nexus's AGPL-3.0 may link with, as the notices name them.
var permitted = map[string]bool{
	"Apache-2.0": true, "BSD-2-Clause": true, "BSD-3-Clause": true, "ISC": true, "MIT": true,
}

var (
	licenceFile = regexp.MustCompile(`(?i)^(licen[cs]e|copying)([._-].*)?$`)
	noticeFile  = regexp.MustCompile(`(?i)^notice([._-].*)?$`)
)

const sectionRule = "================================================================================"

type fileText struct {
	name string
	body string
}

type module struct {
	path, version, dir string
	platforms          []string
	pkgDirs            []string
}

type depRecord struct {
	modPath, modVersion, modDir, pkgDir string
}

// classify names the licence of one licence file's text.
func classify(text string) string {
	t := strings.Join(strings.Fields(text), " ")
	// One file may carry several licences (a module that vendors code under another names both): each is listed.
	var names []string
	if strings.Contains(t, "Apache License") && strings.Contains(t, "Version 2.0") {
		names = append(names, "Apache-2.0")
	}
	if strings.Contains(t, "Mozilla Public License") {
		names = append(names, "MPL-2.0")
	}
	if strings.Contains(t, "GNU GENERAL PUBLIC LICENSE") || strings.Contains(t, "GNU LESSER GENERAL PUBLIC") ||
		strings.Contains(t, "GNU AFFERO") {
		names = append(names, "GPL-family")
	}
	if strings.Contains(t, "Permission is hereby granted, free of charge") {
		names = append(names, "MIT")
	}
	if strings.Contains(t, "Permission to use, copy, modify, and/or distribute this software for any purpose") ||
		strings.Contains(t, "Permission to use, copy, modify, and distribute this software for any purpose") {
		names = append(names, "ISC")
	}
	if strings.Contains(t, "Redistribution and use in source and binary forms") {
		if strings.Contains(t, "Neither the name") || strings.Contains(t, "names of its contributors") ||
			strings.Contains(t, "name of the copyright holder") {
			names = append(names, "BSD-3-Clause")
		} else {
			names = append(names, "BSD-2-Clause")
		}
	}
	return strings.Join(names, " + ")
}

func normalizeBody(path string, raw []byte) (string, error) {
	s := strings.ReplaceAll(string(raw), "\r\n", "\n")
	s = strings.TrimRight(s, "\n")
	if strings.TrimSpace(s) == "" {
		return "", fmt.Errorf("%s: empty licence or notice file", path)
	}
	for _, line := range strings.Split(s, "\n") {
		if line == sectionRule || (strings.HasPrefix(line, "--- ") && strings.HasSuffix(line, " ---")) {
			return "", fmt.Errorf("%s: line %q collides with the licence bundle delimiter", path, line)
		}
	}
	return s + "\n", nil
}

// scanDirs returns the module root ("") followed by every linked package directory (and its parents inside modDir),
// sorted by slash-separated relative path.
func scanDirs(modDir string, pkgDirs []string) ([]string, error) {
	cleanMod := filepath.Clean(modDir)
	seen := map[string]bool{"": true}
	var subs []string
	for _, p := range pkgDirs {
		cur := filepath.Clean(p)
		for {
			rel, err := filepath.Rel(cleanMod, cur)
			if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
				return nil, fmt.Errorf("package directory %s is outside module root %s", p, modDir)
			}
			if rel == "." {
				break
			}
			slash := filepath.ToSlash(rel)
			if !seen[slash] {
				seen[slash] = true
				subs = append(subs, slash)
			}
			parent := filepath.Dir(cur)
			if parent == cur {
				break
			}
			cur = parent
		}
	}
	sort.Strings(subs)
	return append([]string{""}, subs...), nil
}

// licence names a module's licence from the licence files at its root and in any linked package directory, and returns
// the licence and notice files (Apache-2.0 section 4(d) NOTICE files included) with their normalized texts.
func licence(modDir string, pkgDirs []string) (string, []fileText, error) {
	dirs, err := scanDirs(modDir, pkgDirs)
	if err != nil {
		return "", nil, err
	}
	var names []string
	var files []fileText
	for _, relDir := range dirs {
		absDir := modDir
		if relDir != "" {
			absDir = filepath.Join(modDir, filepath.FromSlash(relDir))
		}
		entries, err := os.ReadDir(absDir)
		if err != nil {
			return "", nil, err
		}
		for _, e := range entries {
			if e.IsDir() || strings.HasSuffix(strings.ToLower(e.Name()), ".go") {
				continue
			}
			isLic := licenceFile.MatchString(e.Name())
			isNotice := !isLic && noticeFile.MatchString(e.Name())
			if !isLic && !isNotice {
				continue
			}
			full := filepath.Join(absDir, e.Name())
			b, err := os.ReadFile(full)
			if err != nil {
				return "", nil, err
			}
			body, err := normalizeBody(full, b)
			if err != nil {
				return "", nil, err
			}
			if isLic {
				name := classify(body)
				if name == "" {
					return "", nil, fmt.Errorf("%s: a licence this program cannot name", full)
				}
				for _, part := range strings.Split(name, " + ") {
					if !contains(names, part) {
						names = append(names, part)
					}
				}
			}
			relFile := e.Name()
			if relDir != "" {
				relFile = relDir + "/" + e.Name()
			}
			files = append(files, fileText{name: relFile, body: body})
		}
	}
	if len(names) == 0 {
		return "", nil, fmt.Errorf("%s: no licence file at the module's root", modDir)
	}
	// Several files naming different licences (LICENSE-APACHE beside LICENSE-MIT) are the module's own choice of
	// either; every one is permitted below or the run fails.
	return strings.Join(names, " + "), files, nil
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

// goCommand is the go command that lists the modules (-go): the pinned toolchain's, as cmake/tailscale.cmake passes it.
var goCommand = "go"

// buildTags are the build tags the shim is built with (-tags): the feature omissions of src/tailscale/pins.cmake, as
// cmake/tailscale.cmake passes them. A module only an omitted feature imports is not linked, so not listed.
var buildTags = ""

// ownModules is the path prefix of nexus's own Go modules, which the shim requires by a directory replace.
const ownModules = "github.com/robotgamedevelopers/nexus/"

func deps(goos, goarch string) ([]depRecord, error) {
	cmd := exec.Command(goCommand, "list", "-deps", "-tags="+buildTags, "-f",
		"{{if .Module}}{{if not .Module.Main}}{{.Module.Path}}\t{{.Module.Version}}\t{{if .Module.Replace}}{{.Module.Replace.Dir}}{{else}}{{.Module.Dir}}{{end}}\t{{.Dir}}{{end}}{{end}}",
		"./shim")
	cmd.Env = append(os.Environ(), "GOOS="+goos, "GOARCH="+goarch, "CGO_ENABLED=1")
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("go list for %s/%s: %v\n%s", goos, goarch, err, stderr.String())
	}
	var records []depRecord
	for _, line := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		if line == "" {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 4 {
			return nil, fmt.Errorf("go list printed %q", line)
		}
		records = append(records, depRecord{modPath: f[0], modVersion: f[1], modDir: f[2], pkgDir: f[3]})
	}
	return records, nil
}

func toolchainLicence() (string, error) {
	cmd := exec.Command(goCommand, "env", "GOROOT")
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("go env GOROOT: %v\n%s", err, stderr.String())
	}
	goroot := strings.TrimSpace(string(out))
	if goroot == "" {
		return "", fmt.Errorf("go env GOROOT returned an empty path")
	}
	licPath := filepath.Join(goroot, "LICENSE")
	raw, err := os.ReadFile(licPath)
	if err != nil {
		return "", fmt.Errorf("reading Go toolchain licence %s: %v", licPath, err)
	}
	body, err := normalizeBody(licPath, raw)
	if err != nil {
		return "", err
	}
	if got := classify(body); got != "BSD-3-Clause" {
		return "", fmt.Errorf("%s: classified as %q, want BSD-3-Clause", licPath, got)
	}
	return body, nil
}

func writeSection(b *strings.Builder, header string, files []fileText) {
	b.WriteString("\n")
	b.WriteString(sectionRule + "\n")
	b.WriteString(header + "\n")
	b.WriteString(sectionRule + "\n")
	for _, f := range files {
		b.WriteString("\n--- " + f.name + " ---\n\n")
		b.WriteString(f.body)
	}
}

func renderAll(goVersion, goLicence string, modules map[string]*module) (string, string, error) {
	paths := make([]string, 0, len(modules))
	for path := range modules {
		paths = append(paths, path)
	}
	sort.Strings(paths)

	var rows strings.Builder
	var bundle strings.Builder
	bundle.WriteString("Third-party licences for nexus's built-in Tailscale shim\n")
	bundle.WriteString("Generated by `go run ./notices` in src/tailscale from src/tailscale/NOTICES.md; do not edit by hand.\n")
	writeSection(&bundle, fmt.Sprintf("Go standard library and runtime %s (BSD-3-Clause)", goVersion), []fileText{
		{name: "LICENSE", body: goLicence},
	})

	counts := map[string]int{}
	var refused []string
	for _, path := range paths {
		m := modules[path]
		name, files, err := licence(m.dir, m.pkgDirs)
		if err != nil {
			return "", "", err
		}
		for _, part := range strings.Split(name, " + ") {
			if !permitted[part] {
				refused = append(refused, path+" ("+name+")")
			}
		}
		counts[name]++
		where := "all"
		if len(m.platforms) != len(platforms) {
			where = strings.Join(m.platforms, ", ")
		}
		fileNames := make([]string, len(files))
		for i, f := range files {
			fileNames[i] = f.name
		}
		fmt.Fprintf(&rows, "| %s | %s | %s | %s | %s |\n", m.path, m.version, name, strings.Join(fileNames, ", "), where)
		writeSection(&bundle, fmt.Sprintf("%s@%s (%s)", m.path, m.version, name), files)
	}
	if len(refused) > 0 {
		return "", "", fmt.Errorf("modules nexus (AGPL-3.0) may not link: %s", strings.Join(refused, "; "))
	}
	names := make([]string, 0, len(counts))
	for name := range counts {
		names = append(names, name)
	}
	sort.Strings(names)
	var summary []string
	for _, name := range names {
		summary = append(summary, fmt.Sprintf("%s %d", name, counts[name]))
	}

	var b strings.Builder
	b.WriteString("# Licences of the Go modules the Tailscale shim links\n\n")
	b.WriteString("Generated by `go run ./notices -tags <the omissions>` in `src/tailscale` (do not edit by hand; " +
		"`cmake/tailscale.cmake` runs it): every module that `go list -deps ./shim` names for darwin/arm64, " +
		"darwin/amd64, linux/amd64 and windows/amd64 with cgo on and the build tags the shim is built with (the " +
		"feature omissions in `src/tailscale/pins.cmake`), at the versions `go.mod` pins, with the licence its " +
		"licence files state. CI fails when this file or `src/tailscale/shim/licences.txt` differs from what the " +
		"program writes (`-check`).\n\n")
	fmt.Fprintf(&b, "Toolchain: %s (the Go standard library and runtime, BSD-3-Clause, are linked too). Modules: %d (%s). "+
		"Every one may be combined with nexus's AGPL-3.0.\n\n", goVersion, len(paths), strings.Join(summary, ", "))
	b.WriteString("The licence and notice texts named here, together with the Go standard library's `LICENSE`, are " +
		"written into `src/tailscale/shim/licences.txt`, embedded in every binary that links the shim, and returned " +
		"by `nts_licences` (`src/tailscale/nts.h`).\n\n")
	b.WriteString("| Module | Version | Licence | Licence files | Platforms |\n|---|---|---|---|---|\n")
	b.WriteString(rows.String())
	return b.String(), bundle.String(), nil
}

func render() (string, string, error) {
	goLicence, err := toolchainLicence()
	if err != nil {
		return "", "", err
	}
	modules := map[string]*module{}
	for _, p := range platforms {
		records, err := deps(p.goos, p.goarch)
		if err != nil {
			return "", "", err
		}
		for _, r := range records {
			if strings.HasPrefix(r.modPath, ownModules) {
				// nexus's own code (src/tailscale/lifecycle), under nexus's licence: not a third party's notice.
				continue
			}
			m := modules[r.modPath]
			if m == nil {
				m = &module{path: r.modPath, version: r.modVersion, dir: r.modDir}
				modules[r.modPath] = m
			}
			platform := p.goos + "/" + p.goarch
			if !contains(m.platforms, platform) {
				m.platforms = append(m.platforms, platform)
			}
			if !contains(m.pkgDirs, r.pkgDir) {
				m.pkgDirs = append(m.pkgDirs, r.pkgDir)
			}
		}
	}
	return renderAll(runtime.Version(), goLicence, modules)
}

type parsedSection struct {
	header string
	files  []fileText
}

func parseBundle(bundle string) []parsedSection {
	marker := "\n" + sectionRule + "\n"
	var out []parsedSection
	pos := strings.Index(bundle, marker)
	for pos >= 0 {
		hStart := pos + len(marker)
		relEnd := strings.Index(bundle[hStart:], marker)
		if relEnd < 0 {
			break
		}
		header := bundle[hStart : hStart+relEnd]
		bodyStart := hStart + relEnd + len(marker)
		nextRel := strings.Index(bundle[bodyStart:], marker)
		var body string
		if nextRel < 0 {
			body = bundle[bodyStart:]
			pos = -1
		} else {
			body = bundle[bodyStart : bodyStart+nextRel]
			pos = bodyStart + nextRel
		}
		sec := parsedSection{header: header}
		withNL := "\n" + body
		fpos := strings.Index(withNL, "\n--- ")
		for fpos >= 0 {
			nameStart := fpos + len("\n--- ")
			lineEnd := strings.Index(withNL[nameStart:], " ---\n")
			if lineEnd < 0 {
				break
			}
			fname := withNL[nameStart : nameStart+lineEnd]
			cStart := nameStart + lineEnd + len(" ---\n")
			nextFile := strings.Index(withNL[cStart:], "\n--- ")
			var fbody string
			if nextFile < 0 {
				fbody = withNL[cStart:]
				fpos = -1
			} else {
				fbody = withNL[cStart : cStart+nextFile]
				fpos = cStart + nextFile
			}
			sec.files = append(sec.files, fileText{name: fname, body: strings.TrimSpace(fbody)})
		}
		out = append(out, sec)
	}
	return out
}

// checkAgainstNotices verifies that every module and licence/notice file listed in NOTICES.md is present with
// non-empty text in shim/licences.txt, 1-to-1 in order, with no missing or extra entries.
func checkAgainstNotices(goVersion, noticesMD, licencesTXT string) []string {
	type row struct {
		header string
		files  []string
	}
	var rows []row
	for _, line := range strings.Split(noticesMD, "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "|") || strings.HasPrefix(line, "| Module |") || strings.HasPrefix(line, "|---|") {
			continue
		}
		parts := strings.Split(line, "|")
		if len(parts) != 7 {
			return []string{fmt.Sprintf("NOTICES.md has malformed table row: %q", line)}
		}
		mod := strings.TrimSpace(parts[1])
		ver := strings.TrimSpace(parts[2])
		lic := strings.TrimSpace(parts[3])
		var files []string
		for _, f := range strings.Split(parts[4], ",") {
			if clean := strings.TrimSpace(f); clean != "" {
				files = append(files, clean)
			}
		}
		rows = append(rows, row{header: fmt.Sprintf("%s@%s (%s)", mod, ver, lic), files: files})
	}
	sections := parseBundle(licencesTXT)
	if len(sections) == 0 {
		return []string{"shim/licences.txt holds no licence sections"}
	}
	var problems []string
	wantGo := fmt.Sprintf("Go standard library and runtime %s (BSD-3-Clause)", goVersion)
	if sections[0].header != wantGo {
		problems = append(problems, fmt.Sprintf("shim/licences.txt first section is %q, want %q", sections[0].header, wantGo))
	} else if len(sections[0].files) != 1 || sections[0].files[0].name != "LICENSE" || sections[0].files[0].body == "" {
		problems = append(problems, "shim/licences.txt Go standard library LICENSE text is missing or empty")
	}
	byHeader := map[string]parsedSection{}
	for _, s := range sections[1:] {
		byHeader[s.header] = s
	}
	wantSet := map[string]bool{}
	for _, r := range rows {
		wantSet[r.header] = true
		sec, ok := byHeader[r.header]
		if !ok {
			problems = append(problems, "shim/licences.txt is missing "+r.header)
			continue
		}
		if len(sec.files) != len(r.files) {
			problems = append(problems, fmt.Sprintf("shim/licences.txt %s has %d file(s), NOTICES.md lists %d",
				r.header, len(sec.files), len(r.files)))
			continue
		}
		for i, wantFile := range r.files {
			if sec.files[i].name != wantFile {
				problems = append(problems, fmt.Sprintf("shim/licences.txt %s file %d is %q, want %q",
					r.header, i+1, sec.files[i].name, wantFile))
			}
			if sec.files[i].body == "" {
				problems = append(problems, fmt.Sprintf("shim/licences.txt %s file %q is empty", r.header, wantFile))
			}
		}
	}
	for _, s := range sections[1:] {
		if !wantSet[s.header] {
			problems = append(problems, "shim/licences.txt carries unlisted section "+s.header)
		}
	}
	return problems
}

func main() {
	check := flag.Bool("check", false, "fail if NOTICES.md or shim/licences.txt differs from what would be written")
	flag.StringVar(&goCommand, "go", "go", "the go command to list modules with")
	flag.StringVar(&buildTags, "tags", "", "the build tags the shim is built with, comma-separated")
	flag.Parse()
	licencesPath := filepath.Join("shim", "licences.txt")
	const fixLine = "  fix: cmake --build <build-dir> --target nexus_tailscale_notices " +
		"(or run `GOTOOLCHAIN=go1.27.4 go run ./notices -tags <omissions>` in src/tailscale) and commit both files"
	createdStub := false
	if *check {
		if _, err := os.Stat(licencesPath); err != nil {
			fmt.Fprintln(os.Stderr, "notices: licence files are out of date or incomplete:")
			fmt.Fprintf(os.Stderr, "  cannot read src/tailscale/shim/licences.txt: %v\n", err)
			fmt.Fprintln(os.Stderr, fixLine)
			os.Exit(1)
		}
	} else if _, err := os.Stat(licencesPath); os.IsNotExist(err) {
		// go list ./shim checks //go:embed patterns; a placeholder lets a first generation run.
		if err := os.WriteFile(licencesPath, nil, 0o644); err != nil {
			fmt.Fprintln(os.Stderr, "notices:", err)
			os.Exit(1)
		}
		createdStub = true
	}
	noticesMD, licencesTXT, err := render()
	if err != nil {
		if createdStub {
			_ = os.Remove(licencesPath)
		}
		fmt.Fprintln(os.Stderr, "notices:", err)
		os.Exit(1)
	}
	if *check {
		// A Windows checkout may hold NOTICES.md with CRLF line ends (core.autocrlf); the text is the same.
		var problems []string
		haveNoticesRaw, errN := os.ReadFile("NOTICES.md")
		haveNotices := strings.ReplaceAll(string(haveNoticesRaw), "\r\n", "\n")
		if errN != nil {
			problems = append(problems, fmt.Sprintf("cannot read src/tailscale/NOTICES.md: %v", errN))
		} else if haveNotices != noticesMD {
			problems = append(problems, "src/tailscale/NOTICES.md is not what `go run ./notices` writes at these pins")
		}
		haveLicencesRaw, errL := os.ReadFile(licencesPath)
		haveLicences := strings.ReplaceAll(string(haveLicencesRaw), "\r\n", "\n")
		if errL != nil {
			problems = append(problems, fmt.Sprintf("cannot read src/tailscale/shim/licences.txt: %v", errL))
		} else {
			problems = append(problems, checkAgainstNotices(runtime.Version(), noticesMD, haveLicences)...)
			if haveLicences != licencesTXT {
				problems = append(problems, "src/tailscale/shim/licences.txt is not what `go run ./notices` writes at these pins")
			}
		}
		if len(problems) > 0 {
			fmt.Fprintln(os.Stderr, "notices: licence files are out of date or incomplete:")
			for _, p := range problems {
				fmt.Fprintln(os.Stderr, "  "+p)
			}
			fmt.Fprintln(os.Stderr, fixLine)
			os.Exit(1)
		}
		return
	}
	if err := os.WriteFile("NOTICES.md", []byte(noticesMD), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "notices:", err)
		os.Exit(1)
	}
	if err := os.WriteFile(licencesPath, []byte(licencesTXT), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "notices:", err)
		os.Exit(1)
	}
}
