package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"regexp"
	"sort"
	"strings"
)

const NOISE_THRESHOLD = 0.03

type TestData struct {
	Subs     int `json:"subs"`
	Threads  int `json:"threads"`
	Messages int `json:"messages"`
	Best     int `json:"best"`
	Average  int `json:"average"`
	Worst    int `json:"worst"`
}

type DiffRecord struct {
	Subs          int     `json:"subs"`
	Threads       int     `json:"threads"`
	Messages      int     `json:"messages"`
	BaseAverage   int     `json:"base"`
	BranchAverage int     `json:"branch"`
	Diff          float64 `json:"diff"`
}

type DiffData struct {
	Records []DiffRecord `json:"records"`

	Total struct {
		BestDiff    float64 `json:"best"`
		AverageDiff float64 `json:"average"`
		WorstDiff   float64 `json:"worst"`
	} `json:"total"`
}

func main() {
	flag.Parse()

	if len(os.Args) != 3 {
		log.Fatalf("usage: %s <main> <bench>", os.Args[0])
	}

	m, err := readFile(os.Args[1])
	if err != nil {
		log.Fatal(err)
	}
	b, err := readFile(os.Args[2])
	if err != nil {
		log.Fatal(err)
	}

	diff := map[string]*DiffData{}
	for key := range b {
		diff[key], err = calculateDiff(m[key], b[key])
		if err != nil {
			log.Fatal(err)
		}
	}

	// bb, err := json.MarshalIndent(diff, "", "  ")
	// if err != nil {
	// 	log.Fatal(err)
	// }
	// fmt.Println(string(bb))

	for key, d := range diff {
		fmt.Printf("== %s ==\n", key)
		fmt.Printf("Best: %.2f%%\n", d.Total.BestDiff*100)
		fmt.Printf("Average: %.2f%%\n", d.Total.AverageDiff*100)
		fmt.Printf("Worst: %.2f%%\n", d.Total.WorstDiff*100)
		fmt.Println()
		for _, r := range d.Records {
			fmt.Printf("subs=%d threads=%d messages=%d base=%d branch=%d diff=%.2f%%\n",
				r.Subs, r.Threads, r.Messages, r.BaseAverage, r.BranchAverage, r.Diff*100)
		}
	}
}

func calculateDiff(main, bench []TestData) (*DiffData, error) {
	diff := DiffData{}
	mBestSum, mAverageSum, mWorstSum := 0, 0, 0
	bBestSum, bAverageSum, bWorstSum := 0, 0, 0

	for i := 0; i < len(bench); i++ {
		m := main[i]
		b := bench[i]

		if m.Subs != b.Subs || m.Threads != b.Threads || m.Messages != b.Messages {
			return nil, fmt.Errorf("mismatched data: %v != %v", m, b)
		}

		// Exclude records with less than .5% difference from the output
		d := float64(b.Average-m.Average) / float64(m.Average)
		if d >= NOISE_THRESHOLD || d <= -NOISE_THRESHOLD {
			diff.Records = append(diff.Records, DiffRecord{
				Subs:          m.Subs,
				Threads:       m.Threads,
				Messages:      m.Messages,
				BaseAverage:   m.Average,
				BranchAverage: b.Average,
				Diff:          d,
			})
		}

		mBestSum += m.Best
		mAverageSum += m.Average
		mWorstSum += m.Worst
		bBestSum += b.Best
		bAverageSum += b.Average
		bWorstSum += b.Worst
	}

	sort.Slice(diff.Records, func(i, j int) bool {
		return diff.Records[i].Diff > diff.Records[j].Diff
	})

	diff.Total.WorstDiff = float64(bWorstSum-mWorstSum) / float64(mWorstSum)
	diff.Total.AverageDiff = float64(bAverageSum-mAverageSum) / float64(mAverageSum)
	diff.Total.BestDiff = float64(bBestSum-mBestSum) / float64(mBestSum)
	return &diff, nil
}

func readFile(path string) (map[string][]TestData, error) {
	r, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("failed to open file: %w", err)
	}
	scanner := bufio.NewScanner(r)
	result := make(map[string][]TestData)
	var key string
	re := regexp.MustCompile(`^\d+: (.*)`)

	for scanner.Scan() {
		line := scanner.Text()

		// ignore irrelevant lines, strip the test # prefix
		matches := re.FindStringSubmatch(line)
		if matches == nil {
			continue
		}
		line = matches[1]

		if strings.HasPrefix(line, "== ") && strings.HasSuffix(line, " ==") {
			key = strings.TrimSpace(strings.TrimPrefix(strings.TrimSuffix(line, " =="), "== "))
			continue
		}

		line = strings.TrimPrefix(line, "\x1b[0;0m")
		if strings.HasPrefix(line, "[") {
			var data []TestData
			jsonData := strings.Join([]string{line}, "")
			for scanner.Scan() {
				line := scanner.Text()
				if matches := re.FindStringSubmatch(line); matches != nil {
					line = matches[1]
					jsonData += line
					if strings.HasSuffix(line, "]") {
						break
					}
				}
			}
			if err := json.Unmarshal([]byte(jsonData), &data); err != nil {
				return nil, fmt.Errorf("%s: failed to parse JSON data: %w", path, err)
			}
			if key != "" {
				result[key] = data
			}
		}
	}

	return result, nil
}
