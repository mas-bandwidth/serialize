package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// Every rule STANDARD.md states cites the vectors that prove it (STANDARD.md, "How to read
// this document", adopted with format 2.0 on 2026-10-22). This is the check that holds the
// document to it.
//
// A paragraph states a rule when it uses one of the rule words: must, shall, required,
// refuse, refuses or refused. A list item is a paragraph of its own. Headings, tables and
// code blocks state no rules. A rule paragraph carries [vectors: a, b] naming records in
// conformance/, or [controls: a, b] naming files in conformance-controls/, or both, and
// every name it cites exists. A rule paragraph that cites nothing, and a citation of a name
// the corpus does not hold, are failures. So is a vector no paragraph cites: a vector no
// rule claims proves nothing the document states.

var ruleWord = regexp.MustCompile(`(?i)\b(must|shall|required|refuse|refuses|refused)\b`)
var citation = regexp.MustCompile(`\[(vectors|controls):\s*([^\]]*)\]`)

type paragraph struct {
	line int
	text string
}

// paragraphs splits markdown into the units a citation covers
func paragraphs(text string) []paragraph {
	var out []paragraph
	var current []string
	start := 0
	inFence := false
	flush := func() {
		if len(current) > 0 {
			out = append(out, paragraph{line: start, text: strings.Join(current, " ")})
			current = nil
		}
	}
	lines := strings.Split(text, "\n")
	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "```") {
			flush()
			inFence = !inFence
			continue
		}
		if inFence {
			continue
		}
		if trimmed == "" {
			flush()
			continue
		}
		if len(current) == 0 {
			// a block that opens indented four spaces is code, a table row or a heading is
			// not prose, and none of them states a rule
			if strings.HasPrefix(line, "    ") || strings.HasPrefix(trimmed, "|") || strings.HasPrefix(trimmed, "#") {
				continue
			}
		}
		if strings.HasPrefix(trimmed, "|") || strings.HasPrefix(trimmed, "#") {
			flush()
			continue
		}
		// a list item is its own paragraph
		if strings.HasPrefix(trimmed, "* ") || strings.HasPrefix(trimmed, "- ") || regexp.MustCompile(`^\d+\. `).MatchString(trimmed) {
			flush()
		}
		if len(current) == 0 {
			start = i + 1
		}
		current = append(current, trimmed)
	}
	flush()
	return out
}

// vectorNames reads every `name` in the corpus
func vectorNames(dir string) (map[string]string, error) {
	files, err := filepath.Glob(filepath.Join(dir, "*.txt"))
	if err != nil || len(files) == 0 {
		return nil, fmt.Errorf("no vector files in %s", dir)
	}
	names := map[string]string{}
	for _, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			return nil, err
		}
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "name ") {
				name := strings.TrimSpace(strings.TrimPrefix(line, "name "))
				if previous, ok := names[name]; ok {
					return nil, fmt.Errorf("vector name %q is in both %s and %s: a name is unique across the corpus", name, previous, filepath.Base(f))
				}
				names[name] = filepath.Base(f)
			}
		}
	}
	return names, nil
}

// controlNames reads conformance-controls/: a vector file, an expected-failure list, or a page per control
func controlNames(dir string) map[string]bool {
	names := map[string]bool{}
	for _, pattern := range []string{"*.txt", "*.expected", "*.md"} {
		files, _ := filepath.Glob(filepath.Join(dir, pattern))
		for _, f := range files {
			base := filepath.Base(f)
			names[strings.TrimSuffix(base, filepath.Ext(base))] = true
		}
	}
	return names
}

// checkCitations returns one failure per uncited rule, missing name or orphan vector
func checkCitations(standardPath, corpusDir, controlsDir string) (int, []string) {
	var fails []string
	data, err := os.ReadFile(standardPath)
	if err != nil {
		return 1, []string{err.Error()}
	}
	vectors, err := vectorNames(corpusDir)
	if err != nil {
		return 1, []string{err.Error()}
	}
	controls := controlNames(controlsDir)
	page := filepath.Base(standardPath)
	cited := map[string]bool{}
	checks := 0
	for _, p := range paragraphs(string(data)) {
		cites := citation.FindAllStringSubmatch(p.text, -1)
		prose := citation.ReplaceAllString(p.text, "")
		if ruleWord.MatchString(prose) {
			checks++
			if len(cites) == 0 {
				excerpt := prose
				if len(excerpt) > 90 {
					excerpt = excerpt[:90] + "..."
				}
				fails = append(fails, fmt.Sprintf("%s:%d states a rule and cites no vector: %q", page, p.line, excerpt))
			}
		}
		for _, cite := range cites {
			for _, name := range strings.Split(cite[2], ",") {
				name = strings.TrimSpace(name)
				if name == "" {
					continue
				}
				checks++
				if cite[1] == "vectors" {
					if _, ok := vectors[name]; !ok {
						fails = append(fails, fmt.Sprintf("%s:%d cites vector %q, which the corpus does not hold", page, p.line, name))
					}
					cited[name] = true
				} else if !controls[name] {
					fails = append(fails, fmt.Sprintf("%s:%d cites control %q, which conformance-controls/ does not hold", page, p.line, name))
				}
			}
		}
	}
	var orphans []string
	for name, file := range vectors {
		checks++
		if !cited[name] {
			orphans = append(orphans, fmt.Sprintf("%s (%s)", name, file))
		}
	}
	sort.Strings(orphans)
	for _, o := range orphans {
		fails = append(fails, fmt.Sprintf("vector %s is cited by no rule in %s", o, page))
	}
	return checks, fails
}

// runCitations is the check inside the full run
func runCitations(c *checker, root string) {
	standard := filepath.Join(root, "STANDARD.md")
	if _, err := os.Stat(standard); err != nil {
		root = filepath.Join(root, "..", "..")
		standard = filepath.Join(root, "STANDARD.md")
	}
	n, fails := checkCitations(standard, filepath.Join(root, "conformance"), filepath.Join(root, "conformance-controls"))
	c.n += n
	c.fails = append(c.fails, fails...)
}

// citationsMain is `go run . --citations <standard.md>`: the citation check alone, against a
// document named on the command line, which is how the negative control runs it against a
// page with an uncited rule
func citationsMain(standard string) {
	root, _ := filepath.Abs(".")
	corpus := filepath.Join(root, "conformance")
	if _, err := os.Stat(corpus); err != nil {
		root = filepath.Join(root, "..", "..")
		corpus = filepath.Join(root, "conformance")
	}
	n, fails := checkCitations(standard, corpus, filepath.Join(root, "conformance-controls"))
	fmt.Printf("%d citation checks against %s, %d failures\n", n, standard, len(fails))
	for _, f := range fails {
		fmt.Println("  FAIL " + f)
	}
	if len(fails) > 0 {
		os.Exit(1)
	}
}
