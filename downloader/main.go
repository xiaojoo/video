package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

func main() {
	mode := flag.String("mode", "download", "download | seed")
	dataDir := flag.String("data", "", "directory the downloaded file lands in")
	stateDir := flag.String("state", "", "directory for metadata and piece-completion state (default: -data)")
	src := flag.String("src", "", "magnet URI or path to a .torrent file")
	file := flag.String("file", "", "file to seed (seed mode)")
	seedPort := flag.Int("port", 0, "listen port for seed mode (0 = let the OS pick; the address is printed)")
	peers := flag.String("peers", "", "comma-separated host:port peers to dial directly")
	isolated := flag.Bool("isolated", false, "no DHT and no trackers; rely on -peers only")
	httpAddr := flag.String("http", "127.0.0.1:0", "loopback address to serve the seekable HTTP view on")
	readaheadMB := flag.Float64("readahead", 4, "how far ahead of a reader to prioritise, in MiB")
	want := flag.String("want", "all", "all = fetch every piece | stream = fetch only what a reader asks for")
	limitMB := flag.Float64("limit", 0, "download rate cap in MiB/s (0 = uncapped)")
	seed := flag.Bool("seed", false, "keep uploading once the torrent is complete")
	interval := flag.Duration("interval", time.Second, "status line interval")
	verbose := flag.Bool("v", false, "turn on the engine's own debug logging")
	rawURL := flag.String("url", "", "direct http(s) media url or HLS playlist (http mode)")
	outName := flag.String("out-name", "", "file name to save under (default: the url's last path segment)")
	headers := flag.String("headers", "", "file of HTTP headers to send, one 'Name: value' per line (keeps cookies out of argv)")
	conns := flag.Int("conns", 8, "parallel connections for an http job")
	chunkMB := flag.Float64("chunk", 8, "bytes per part in MiB for an http job")
	flag.Parse()

	if *mode == "http" {
		if *rawURL == "" {
			die("http mode needs -url")
		}
		if *dataDir == "" {
			*dataDir = "."
		}
		var hdr map[string]string
		if *headers != "" {
			h, err := readHeadersFile(*headers)
			if err != nil {
				die("headers: %v", err)
			}
			hdr = h
		}
		name := *outName
		if name == "" {
			name = nameFromURL(*rawURL)
		}
		logf := func(format string, a ...any) { fmt.Fprintf(os.Stderr, format+"\n", a...) }
		job, err := newHttpJob(*rawURL, filepath.Join(*dataDir, name), hdr, *chunkMB, logf)
		if err != nil {
			die("%v", err)
		}
		quit := make(chan struct{})
		srv, err := serveHTTPJob(job, *httpAddr, quit)
		if err != nil {
			die("http: %v", err)
		}
		fmt.Printf("LISTEN %s\n", srv.URL)
		os.Stdout.Sync()

		errc := make(chan error, 1)
		go func() { errc <- job.run(*conns, quit) }()

		done := make(chan os.Signal, 1)
		signal.Notify(done, os.Interrupt, syscall.SIGTERM)
		tick := time.NewTicker(*interval)
		defer tick.Stop()
		for {
			select {
			case err := <-errc:
				if err != nil {
					fmt.Printf("ERROR %v\n", err)
					os.Stdout.Sync()
					die("%v", err)
				}
				if !job.isComplete() {
					die("http job ended with %d/%d parts", job.status().PiecesDone, len(job.Parts))
				}
				if err := job.Finish(); err != nil {
					die("finish: %v", err)
				}
				job.waitViews(5 * time.Second)
				// After Finish, so the record says complete and names the file that exists.
				b, _ := json.Marshal(job.status())
				fmt.Printf("DONE %s\n", b)
				os.Stdout.Sync()
				return
			case <-tick.C:
				b, _ := json.Marshal(job.status())
				fmt.Printf("%s\n", b)
				os.Stdout.Sync()
			case <-quit:
				// /quit asked for a clean stop: give the workers a moment to finish
				// their in-flight block so the state file describes written bytes.
				select {
				case <-errc:
				case <-time.After(3 * time.Second):
				}
				b, _ := json.Marshal(job.status())
				fmt.Printf("EXIT %s\n", b)
				os.Stdout.Sync()
				return
			case <-done:
				close(quit)
				b, _ := json.Marshal(job.status())
				fmt.Printf("EXIT %s\n", b)
				os.Stdout.Sync()
				return
			}
		}
	}

	if *mode == "seed" {
		if *file == "" {
			die("seed mode needs -file")
		}
		if err := runSeed(*file, *seedPort); err != nil {
			die("seed: %v", err)
		}
		return
	}

	if *dataDir == "" || *src == "" {
		fmt.Fprintln(os.Stderr, "usage:")
		fmt.Fprintln(os.Stderr, "  downloader -mode download -data DIR [-state DIR] -src MAGNET|FILE.torrent [-http ADDR] [-limit MiB/s] [-want all|stream] [-peers h:p,h:p] [-isolated] [-seed]")
		fmt.Fprintln(os.Stderr, "  downloader -mode http -data DIR -url URL [-out-name NAME] [-headers FILE] [-conns N] [-chunk MiB] [-http ADDR]")
		fmt.Fprintln(os.Stderr, "  downloader -mode seed -file FILE [-port 0]")
		os.Exit(2)
	}

	if *stateDir == "" {
		*stateDir = *dataDir
	}

	eng, err := newEngine(*dataDir, *stateDir, *limitMB, *seed, *verbose, *isolated)
	if err != nil {
		die("engine: %v", err)
	}
	defer eng.close()

	if err := eng.start(*src, *peers, *want); err != nil {
		die("start: %v", err)
	}

	srv, err := serveHTTP(*httpAddr, eng, int64(*readaheadMB*1024*1024))
	if err != nil {
		die("http: %v", err)
	}
	fmt.Printf("LISTEN %s\n", srv.URL)
	os.Stdout.Sync()

	done := make(chan os.Signal, 1)
	signal.Notify(done, os.Interrupt, syscall.SIGTERM)

	tick := time.NewTicker(*interval)
	defer tick.Stop()
	shutdown := func(prefix string) {
		// Snapshot before the drop: after Close the torrent is out of the client and
		// reading its counters back reports an empty job.
		final, _ := json.Marshal(eng.status())
		eng.close()
		fmt.Printf("%s %s\n", prefix, final)
		os.Stdout.Sync()
	}
	for {
		select {
		case <-tick.C:
			st := eng.status()
			b, _ := json.Marshal(st)
			fmt.Printf("%s\n", b)
			os.Stdout.Sync()
			if st.Complete && !*seed {
				// Nothing left to upload, so the process that existed to fetch the
				// file has done its job.
				shutdown("DONE")
				return
			}
		case <-srv.Quit:
			shutdown("EXIT")
			return
		case <-done:
			shutdown("EXIT")
			return
		case <-srv.Stopped:
			eng.close()
			die("http server stopped")
		}
	}
}

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "ERROR "+format+"\n", a...)
	os.Exit(1)
}
