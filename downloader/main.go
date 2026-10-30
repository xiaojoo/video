package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
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
	flag.Parse()

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
