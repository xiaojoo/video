package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/anacrolix/torrent"
)

// The HTTP view is the seam this design hangs on: mpv already plays an http URL
// with Range requests, so a torrent that is still missing most of its bytes
// becomes playable by answering those ranges from the piece store instead of
// from a file. Reads through a Reader are what tell the engine which pieces the
// play head wants next.
type Server struct {
	URL     string
	Stopped chan struct{}
	// Quit is how a supervisor asks for a clean shutdown. On Windows a signal sent
	// from a shell does not reach a native process, and an unclean exit is exactly
	// what resume state must not be left half-written.
	Quit    chan struct{}
	srv     *http.Server
	quitOne sync.Once
}

func serveHTTP(addr string, e *Engine, readahead int64) (*Server, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	srv := &http.Server{ReadHeaderTimeout: 10 * time.Second}
	s := &Server{
		Stopped: make(chan struct{}),
		Quit:    make(chan struct{}),
		srv:     srv,
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /status", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(e.status())
	})
	mux.HandleFunc("GET /pieces", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(e.pieceRuns())
	})
	mux.HandleFunc("POST /quit", func(w http.ResponseWriter, r *http.Request) {
		s.quitOne.Do(func() {
			close(s.Quit)
			fmt.Println("QUIT")
		})
		fmt.Fprintln(w, "quitting")
	})
	mux.HandleFunc("POST /want/{mode}", func(w http.ResponseWriter, r *http.Request) {
		n, err := e.setWant(r.PathValue("mode"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		fmt.Fprintf(w, "wanted %d\n", n)
	})
	mux.HandleFunc("GET /file/{index}", func(w http.ResponseWriter, r *http.Request) {
		idx, err := strconv.Atoi(r.PathValue("index"))
		if err != nil {
			http.Error(w, "bad index", http.StatusBadRequest)
			return
		}
		file, err := e.fileAt(idx)
		if err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		serveFile(w, r, file, idx, readahead)
	})
	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		st := e.status()
		fmt.Fprintf(w, "%s  %s\n", st.Hash, st.Name)
		fmt.Fprintf(w, "total=%d completed=%d pct=%.2f rate=%.2fMB/s peers=%d\n\n",
			st.Total, st.Completed, st.Pct, st.Rate/1024/1024, st.Peers)
		for _, f := range st.Files {
			fmt.Fprintf(w, "/file/%d\t%d bytes\t%.2f%%\t%s\n", f.Index, f.Length, f.Pct, f.Path)
		}
	})

	srv.Handler = mux
	go func() {
		_ = srv.Serve(ln)
		close(s.Stopped)
	}()
	s.URL = "http://" + ln.Addr().String()
	return s, nil
}

func (e *Engine) fileAt(idx int) (*torrent.File, error) {
	t := e.t
	if t == nil {
		return nil, fmt.Errorf("no torrent")
	}
	files := t.Files()
	if idx < 0 || idx >= len(files) {
		return nil, fmt.Errorf("index %d out of range (0..%d)", idx, len(files)-1)
	}
	return files[idx], nil
}

func serveFile(w http.ResponseWriter, r *http.Request, f *torrent.File, idx int, readahead int64) {
	total := f.Length()
	w.Header().Set("Accept-Ranges", "bytes")
	w.Header().Set("Content-Type", "application/octet-stream")

	var start, end int64 = 0, total - 1
	partial := false
	if rng := r.Header.Get("Range"); rng != "" {
		s, e2, ok := parseRange(rng, total)
		if !ok {
			w.Header().Set("Content-Range", fmt.Sprintf("bytes */%d", total))
			http.Error(w, "unsatisfiable range", http.StatusRequestedRangeNotSatisfiable)
			return
		}
		start, end, partial = int64(s), int64(e2), true
	}

	rd := f.NewReader()
	defer rd.Close()
	// Responsive: a read returns as soon as the bytes are on disk rather than
	// waiting for the whole piece to hash, which is what makes a 15 %-complete
	// file playable. Readahead is what pulls the pieces just ahead of the play
	// head, so the next range mpv asks for is already wanted by the time it asks.
	rd.SetResponsive()
	rd.SetReadahead(readahead)
	rd.SetContext(r.Context())

	if start > 0 {
		if _, err := rd.Seek(start, io.SeekStart); err != nil {
			http.Error(w, "seek: "+err.Error(), http.StatusInternalServerError)
			return
		}
	}
	n := int64(end - start + 1)
	if partial {
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, end, total))
		w.WriteHeader(http.StatusPartialContent)
	}
	w.(http.Flusher).Flush()

	t0 := time.Now()
	copied, err := io.CopyN(w, rd, n)
	if err != nil && err != io.EOF && r.Context().Err() == nil {
		// A stalled read here is the play head waiting on the swarm; the client
		// hanging up is the normal end of that wait, not a fault to hide.
		fmt.Printf("READERR index=%d start=%d copied=%d elapsedMs=%d err=%v\n",
			idx, start, copied, time.Since(t0).Milliseconds(), err)
	}
}

// parseRange accepts the forms mpv sends: "bytes=a-b", "bytes=a-" and
// "bytes=-n". Multi-range requests are rejected rather than half-served, because
// a wrong byte window into a video is a crash, not a cosmetic fault.
func parseRange(h string, total int64) (int, int, bool) {
	if !strings.HasPrefix(h, "bytes=") {
		return 0, 0, false
	}
	spec := strings.Split(strings.TrimPrefix(h, "bytes="), ",")
	if len(spec) != 1 {
		return 0, 0, false
	}
	a, b, ok := strings.Cut(spec[0], "-")
	if !ok {
		return 0, 0, false
	}
	var start, end int64
	switch {
	case a == "" && b == "":
		return 0, 0, false
	case a == "":
		n, err := strconv.ParseInt(b, 10, 64)
		if err != nil || n <= 0 || n > total {
			return 0, 0, false
		}
		start, end = total-n, total-1
	default:
		var err error
		if start, err = strconv.ParseInt(a, 10, 64); err != nil || start < 0 || start >= total {
			return 0, 0, false
		}
		if b == "" {
			end = total - 1
		} else if end, err = strconv.ParseInt(b, 10, 64); err != nil || end < start {
			return 0, 0, false
		}
	}
	if end > total-1 {
		end = total - 1
	}
	return int(start), int(end), true
}
