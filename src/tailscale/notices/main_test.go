package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const (
	sampleMIT = "Permission is hereby granted, free of charge, to any person obtaining a copy of this software.\n"
	sampleApache = "Apache License\nVersion 2.0, January 2004\nhttp://www.apache.org/licenses/\n"
	sampleBSD3 = "Redistribution and use in source and binary forms, with or without modification.\n" +
		"Neither the name of the project nor the names of its contributors may be used.\n"
)

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestRenderAndCheckNoticesBundle(t *testing.T) {
	root := t.TempDir()
	modApache := filepath.Join(root, "mod-apache")
	writeTestFile(t, filepath.Join(modApache, "LICENSE"), sampleApache)
	writeTestFile(t, filepath.Join(modApache, "NOTICE"), "Example Apache 4(d) attribution notice.\r\n")

	modSub := filepath.Join(root, "mod-sub")
	writeTestFile(t, filepath.Join(modSub, "LICENSE"), sampleBSD3)
	writeTestFile(t, filepath.Join(modSub, "internal", "xxhash", "LICENSE.txt"), sampleMIT+"\r\n")
	// An unlinked subpackage's licence file is not scanned.
	writeTestFile(t, filepath.Join(modSub, "unused", "LICENSE"), "GNU GENERAL PUBLIC LICENSE\n")

	mods := map[string]*module{
		"example.com/apache": {
			path:      "example.com/apache",
			version:   "v1.2.3",
			dir:       modApache,
			platforms: []string{"darwin/arm64", "darwin/amd64", "linux/amd64", "windows/amd64"},
			pkgDirs:   []string{modApache},
		},
		"example.com/sub": {
			path:      "example.com/sub",
			version:   "v0.4.0",
			dir:       modSub,
			platforms: []string{"linux/amd64"},
			pkgDirs:   []string{modSub, filepath.Join(modSub, "internal", "xxhash")},
		},
	}

	noticesMD, licencesTXT, err := renderAll("go1.27.4", sampleBSD3, mods)
	if err != nil {
		t.Fatalf("renderAll: %v", err)
	}
	if strings.Contains(licencesTXT, "\r") {
		t.Fatal("licencesTXT contains CR")
	}
	if !strings.Contains(noticesMD, "| example.com/apache | v1.2.3 | Apache-2.0 | LICENSE, NOTICE | all |") {
		t.Fatalf("Apache NOTICE not listed in NOTICES.md:\n%s", noticesMD)
	}
	if !strings.Contains(noticesMD, "| example.com/sub | v0.4.0 | BSD-3-Clause + MIT | LICENSE, internal/xxhash/LICENSE.txt | linux/amd64 |") {
		t.Fatalf("subpackage LICENSE.txt not listed in NOTICES.md:\n%s", noticesMD)
	}
	if !strings.Contains(licencesTXT, "Example Apache 4(d) attribution notice.") {
		t.Fatal("Apache NOTICE body missing from licencesTXT")
	}
	if probs := checkAgainstNotices("go1.27.4", noticesMD, licencesTXT); len(probs) != 0 {
		t.Fatalf("unexpected problems on clean output: %v", probs)
	}

	// Guard checks: missing module, empty file text, and unlisted section must each fail.
	cutIdx := strings.Index(licencesTXT, "\n"+sectionRule+"\nexample.com/sub@v0.4.0")
	if cutIdx < 0 {
		t.Fatal("missing example.com/sub section")
	}
	probs := checkAgainstNotices("go1.27.4", noticesMD, licencesTXT[:cutIdx])
	if !strings.Contains(strings.Join(probs, "\n"), "missing example.com/sub@v0.4.0 (BSD-3-Clause + MIT)") {
		t.Fatalf("expected missing module error, got: %v", probs)
	}

	emptied := strings.Replace(licencesTXT, "Example Apache 4(d) attribution notice.\n", "   \n", 1)
	probs = checkAgainstNotices("go1.27.4", noticesMD, emptied)
	if !strings.Contains(strings.Join(probs, "\n"), `file "NOTICE" is empty`) {
		t.Fatalf("expected empty NOTICE error, got: %v", probs)
	}

	extra := licencesTXT + "\n" + sectionRule + "\nexample.com/extra@v0.1.0 (MIT)\n" + sectionRule + "\n\n--- LICENSE ---\n\n" + sampleMIT
	probs = checkAgainstNotices("go1.27.4", noticesMD, extra)
	if !strings.Contains(strings.Join(probs, "\n"), "unlisted section example.com/extra@v0.1.0 (MIT)") {
		t.Fatalf("expected unlisted section error, got: %v", probs)
	}
}

func TestRefusesIncompatibleLicence(t *testing.T) {
	root := t.TempDir()
	modGPL := filepath.Join(root, "mod-gpl")
	writeTestFile(t, filepath.Join(modGPL, "COPYING"), "GNU GENERAL PUBLIC LICENSE\n")
	mods := map[string]*module{
		"example.com/gpl": {
			path:      "example.com/gpl",
			version:   "v1.0.0",
			dir:       modGPL,
			platforms: []string{"linux/amd64"},
			pkgDirs:   []string{modGPL},
		},
	}
	if _, _, err := renderAll("go1.27.4", sampleBSD3, mods); err == nil || !strings.Contains(err.Error(), "may not link") {
		t.Fatalf("expected GPL refusal, got: %v", err)
	}
}
