// Command notices writes src/tailscale/NOTICES.md: every Go module the shim (src/tailscale/shim) links on each
// platform nexus builds it for, with the licence its licence file states, measured by `go list -deps` per platform. It
// fails on a module whose licence it cannot name, or one nexus (AGPL-3.0) cannot combine with.
//
//	go run ./notices          (from src/tailscale) rewrites NOTICES.md
//	go run ./notices -check   fails if NOTICES.md is not what it would write
//	  -go <path>              the go command that lists the modules (default: go on PATH)
//
// Run it with the pinned toolchain (GOTOOLCHAIN=go1.27.1, as cmake/tailscale.cmake does) after any change to go.mod.
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

var licenceFile = regexp.MustCompile(`(?i)^(licen[cs]e|copying)([._-].*)?$`)

type module struct {
	path, version, dir string
	platforms          []string
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

// licence names a module's licence from the licence files at its root, and the files it read.
func licence(dir string) (string, []string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return "", nil, err
	}
	var names, files []string
	for _, e := range entries {
		if e.IsDir() || !licenceFile.MatchString(e.Name()) || strings.HasSuffix(e.Name(), ".go") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			return "", nil, err
		}
		name := classify(string(b))
		if name == "" {
			return "", nil, fmt.Errorf("%s: a licence this program cannot name", filepath.Join(dir, e.Name()))
		}
		files = append(files, e.Name())
		for _, part := range strings.Split(name, " + ") {
			if !contains(names, part) {
				names = append(names, part)
			}
		}
	}
	if len(names) == 0 {
		return "", nil, fmt.Errorf("%s: no licence file at the module's root", dir)
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

func deps(goos, goarch string) ([]string, error) {
	cmd := exec.Command(goCommand, "list", "-deps", "-f",
		"{{with .Module}}{{if not .Main}}{{.Path}}\t{{.Version}}\t{{if .Replace}}{{.Replace.Dir}}{{else}}{{.Dir}}{{end}}{{end}}{{end}}",
		"./shim")
	cmd.Env = append(os.Environ(), "GOOS="+goos, "GOARCH="+goarch, "CGO_ENABLED=1")
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("go list for %s/%s: %v\n%s", goos, goarch, err, stderr.String())
	}
	return strings.Split(strings.TrimSpace(string(out)), "\n"), nil
}

func render() (string, error) {
	modules := map[string]*module{}
	for _, p := range platforms {
		lines, err := deps(p.goos, p.goarch)
		if err != nil {
			return "", err
		}
		for _, line := range lines {
			if line == "" {
				continue
			}
			f := strings.Split(line, "\t")
			if len(f) != 3 {
				return "", fmt.Errorf("go list printed %q", line)
			}
			m := modules[f[0]]
			if m == nil {
				m = &module{path: f[0], version: f[1], dir: f[2]}
				modules[f[0]] = m
			}
			platform := p.goos + "/" + p.goarch
			if !contains(m.platforms, platform) {
				m.platforms = append(m.platforms, platform)
			}
		}
	}
	paths := make([]string, 0, len(modules))
	for path := range modules {
		paths = append(paths, path)
	}
	sort.Strings(paths)

	var rows strings.Builder
	counts := map[string]int{}
	var refused []string
	for _, path := range paths {
		m := modules[path]
		name, files, err := licence(m.dir)
		if err != nil {
			return "", err
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
		fmt.Fprintf(&rows, "| %s | %s | %s | %s | %s |\n", m.path, m.version, name, strings.Join(files, ", "), where)
	}
	if len(refused) > 0 {
		return "", fmt.Errorf("modules nexus (AGPL-3.0) may not link: %s", strings.Join(refused, "; "))
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
	b.WriteString("Generated by `go run ./notices` in `src/tailscale` (do not edit by hand): every module that `go list " +
		"-deps ./shim` names for darwin/arm64, darwin/amd64, linux/amd64 and windows/amd64 with cgo on, at the " +
		"versions `go.mod` pins, with the licence its licence files state. CI fails when this file differs from what " +
		"the program writes (`go run ./notices -check`).\n\n")
	fmt.Fprintf(&b, "Toolchain: %s (the Go standard library and runtime, BSD-3-Clause, are linked too). Modules: %d (%s). "+
		"Every one may be combined with nexus's AGPL-3.0.\n\n", runtime.Version(), len(paths), strings.Join(summary, ", "))
	b.WriteString("A binary that links the shim carries each module's licence file named here, from the module at its " +
		"version.\n\n")
	b.WriteString("| Module | Version | Licence | Licence files | Platforms |\n|---|---|---|---|---|\n")
	b.WriteString(rows.String())
	return b.String(), nil
}

func main() {
	check := flag.Bool("check", false, "fail if NOTICES.md differs from what would be written")
	flag.StringVar(&goCommand, "go", "go", "the go command to list modules with")
	flag.Parse()
	text, err := render()
	if err != nil {
		fmt.Fprintln(os.Stderr, "notices:", err)
		os.Exit(1)
	}
	if *check {
		// A Windows checkout may hold the file with CRLF line ends (core.autocrlf); the text is the same.
		have, err := os.ReadFile("NOTICES.md")
		if err != nil || strings.ReplaceAll(string(have), "\r\n", "\n") != text {
			fmt.Fprintln(os.Stderr, "notices: src/tailscale/NOTICES.md is not what `go run ./notices` writes at these "+
				"pins; run it in src/tailscale with GOTOOLCHAIN=go1.27.1 and commit the file")
			os.Exit(1)
		}
		return
	}
	if err := os.WriteFile("NOTICES.md", []byte(text), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "notices:", err)
		os.Exit(1)
	}
}
