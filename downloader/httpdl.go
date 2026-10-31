package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// The HTTP engine answers the same two things the torrent engine does: /status with
// the Status JSON, and /file/0 as a seekable view of the bytes that exist. The shell
// reads LISTEN, /status and playUrl and nothing else, so a second engine that invents
// its own protocol is a second thing to keep in step for no gain.
//
// Parts are handed out in offset order, never in completion order. That is what makes
// watching a partial download possible at all: an arbitrary fill order leaves holes,
// and a hole in the middle has no honest answer to a range request. Ordered
// allocation keeps every committed byte inside one contiguous prefix, so the view can
// wait for the play head's window and then serve it.

type part struct {
	url   string // playlist segments carry their own url; a split file reuses the job url
	start int64
	size  int64
}

type HttpJob struct {
	URL     string
	Headers map[string]string
	Out     string
	Tmp     string
	Parts   []part
	m3u8    bool // segments are whole files, fetched without Range

	mu        sync.Mutex
	total     int64
	name      string
	done      []bool
	prefixIdx int
	prefix    int64
	completed int64
	written   int64
	rate      float64
	complete  bool // every part landed
	finalized bool // renamed into place; only then is the file at Out playable
	started   time.Time
	resume    string

	next atomic.Int64
	// live counts connections with a request in flight; the shell shows it where the
	// torrent engine shows peers, and a made-up number there reads as progress.
	live atomic.Int32
	// views counts /file/0 responses. Exiting the process the instant the last part
	// lands cuts a reader off mid-body, which looks like a corrupt file to mpv.
	views atomic.Int32
	logf  func(string, ...any)
}

func firstRunes(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n]) + "…"
	}
	return s
}

// readHeadersFile keeps credentials out of argv, where the process list would show
// them to anything running on the box.
func readHeadersFile(p string) (map[string]string, error) {
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	h := map[string]string{}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, ":")
		if !ok {
			return nil, fmt.Errorf("headers 行缺冒号: %q", firstRunes(line, 24))
		}
		h[strings.TrimSpace(k)] = strings.TrimSpace(v)
	}
	return h, nil
}

func nameFromURL(raw string) string {
	u, err := url.Parse(raw)
	if err != nil || u.Path == "" {
		return "download"
	}
	base := filepath.Base(u.Path)
	if base == "." || base == "/" || base == "" {
		return "download"
	}
	return base
}

func (j *HttpJob) newReq(raw string) (*http.Request, error) {
	req, err := http.NewRequest("GET", raw, nil)
	if err != nil {
		return nil, err
	}
	for k, v := range j.Headers {
		req.Header.Set(k, v)
	}
	return req, nil
}

// probe asks what the server says about this url: length, whether ranges work, and
// whether the payload is an HLS playlist to begin with. A non-2xx is not "a short
// file": share links answer 403 and expired ones answer 200 with a login page, and
// treating either as content is how a download ends up being 10 bytes of JSON.
func (j *HttpJob) probe(raw string) (size int64, accepts bool, ctype string, body []byte, err error) {
	req, err := j.newReq(raw)
	if err != nil {
		return 0, false, "", nil, err
	}
	req.Header.Set("Range", "bytes=0-0")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return 0, false, "", nil, err
	}
	defer resp.Body.Close()
	ctype = resp.Header.Get("Content-Type")
	accepts = resp.StatusCode == 206 || resp.Header.Get("Accept-Ranges") == "bytes"
	if cr := resp.Header.Get("Content-Range"); cr != "" {
		if _, after, ok := strings.Cut(cr, "/"); ok {
			size, _ = strconv.ParseInt(strings.TrimSpace(after), 10, 64)
		}
	}
	if size <= 0 {
		size = resp.ContentLength
	}
	body, _ = io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusPartialContent {
		return 0, false, ctype, body, fmt.Errorf("http %d %s", resp.StatusCode, firstRunes(strings.TrimSpace(string(body)), 80))
	}
	return size, accepts, ctype, body, nil
}

func looksLikePage(ctype string, body []byte) bool {
	if strings.Contains(strings.ToLower(ctype), "text/html") {
		return true
	}
	t := strings.TrimSpace(strings.ToLower(string(body)))
	return strings.HasPrefix(t, "<!doctype") || strings.HasPrefix(t, "<html")
}

// segmentList resolves a playlist's segment lines against the playlist's own url:
// the shared-preview lists hand back relative paths, so joining is not optional.
func segmentList(text, base string) ([]string, error) {
	u, err := url.Parse(base)
	if err != nil {
		return nil, err
	}
	var out []string
	sc := bufio.NewScanner(strings.NewReader(text))
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		rel, err := url.Parse(l)
		if err != nil {
			continue
		}
		out = append(out, u.ResolveReference(rel).String())
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("播放列表里没有片段行")
	}
	return out, nil
}

// headSweep gets every segment's length up front so total and the prefix are real
// numbers instead of a running guess. Some CDNs answer 405 to HEAD, so a size is
// re-asked with a one-byte ranged GET before giving up: the segment headers alone
// say how long it is, and closing the body keeps the cost at a few hundred bytes.
func (j *HttpJob) headSweep(segs []string) []int64 {
	sizes := make([]int64, len(segs))
	var wg sync.WaitGroup
	gate := make(chan struct{}, 16)
	for i, s := range segs {
		wg.Add(1)
		go func(i int, s string) {
			defer wg.Done()
			gate <- struct{}{}
			defer func() { <-gate }()
			req, err := j.newReq(s)
			if err != nil {
				return
			}
			req.Method = "HEAD"
			resp, err := http.DefaultClient.Do(req)
			if err == nil {
				resp.Body.Close()
				if resp.StatusCode == http.StatusOK && resp.ContentLength > 0 {
					sizes[i] = resp.ContentLength
					return
				}
			}
			req, err = j.newReq(s)
			if err != nil {
				return
			}
			req.Header.Set("Range", "bytes=0-0")
			resp, err = http.DefaultClient.Do(req)
			if err != nil {
				return
			}
			resp.Body.Close()
			if cr := resp.Header.Get("Content-Range"); cr != "" {
				if _, after, ok := strings.Cut(cr, "/"); ok {
					sizes[i], _ = strconv.ParseInt(strings.TrimSpace(after), 10, 64)
				}
			}
			if sizes[i] <= 0 && resp.StatusCode == http.StatusOK {
				sizes[i] = resp.ContentLength
			}
		}(i, s)
	}
	wg.Wait()
	return sizes
}

func newHttpJob(rawURL, out string, hdr map[string]string, chunkMiB float64, logf func(string, ...any)) (*HttpJob, error) {
	j := &HttpJob{URL: rawURL, Headers: hdr, Out: out, Tmp: out + ".part", logf: logf, started: time.Now()}
	if chunkMiB <= 0 {
		chunkMiB = 8
	}
	size, accepts, ctype, body, err := j.probe(rawURL)
	if err != nil {
		// The row already shows the url; repeating it here is what makes the error line
		// run off the end of the card.
		return nil, fmt.Errorf("探测地址失败: %w", err)
	}
	if strings.Contains(ctype, "mpegurl") || strings.HasPrefix(string(body), "#EXTM3U") {
		cur, curBody := rawURL, string(body)
		var segs []string
		for depth := 0; ; depth++ {
			if depth > 3 {
				return nil, fmt.Errorf("播放列表嵌套超过 4 层")
			}
			s, err := segmentList(curBody, cur)
			if err != nil {
				return nil, err
			}
			// A master playlist's lines point at other playlists; stitching those in
			// as media would produce a file made of text.
			if !strings.Contains(curBody, "#EXT-X-STREAM-INF") {
				segs = s
				break
			}
			_, _, _, vb, err := j.probe(s[0])
			if err != nil {
				return nil, fmt.Errorf("变体列表 %s: %w", s[0], err)
			}
			cur, curBody = s[0], string(vb)
		}
		sizes := j.headSweep(segs)
		var off int64
		for i, s := range segs {
			if sizes[i] <= 0 {
				return nil, fmt.Errorf("片段 %d 没有 content-length，无法按序拼接", i)
			}
			j.Parts = append(j.Parts, part{url: s, start: off, size: sizes[i]})
			off += sizes[i]
		}
		j.m3u8, j.total, j.name = true, off, nameFromURL(rawURL)
		logf("HLS 片数=%d 总字节=%d range=%v", len(j.Parts), j.total, accepts)
	} else {
		if size <= 0 {
			return nil, fmt.Errorf("服务器不给文件大小（accept-ranges=%v type=%s）", accepts, ctype)
		}
		if looksLikePage(ctype, body) {
			return nil, fmt.Errorf("返回的是网页不是媒体（链接失效或需要登录）")
		}
		j.total, j.name = size, nameFromURL(rawURL)
		per0 := int64(chunkMiB * 1024 * 1024)
		n := int((size + per0 - 1) / per0)
		if !accepts {
			n = 1
		}
		per := (size + int64(n) - 1) / int64(n)
		for i := 0; i < n; i++ {
			s := int64(i) * per
			if s >= size {
				break
			}
			sz := min(per, size-s)
			j.Parts = append(j.Parts, part{url: rawURL, start: s, size: sz})
		}
		logf("单文件 字节=%d 分块=%d range=%v", j.total, len(j.Parts), accepts)
	}
	j.done = make([]bool, len(j.Parts))
	return j, nil
}

func (j *HttpJob) bytesOf(i int) int64 { return j.Parts[i].size }

// statePath sits next to the file so a moved download directory cannot resurrect a
// stale bitmap against unrelated bytes.
func (j *HttpJob) statePath() string { return j.Tmp + ".json" }

type httpState struct {
	URL    string `json:"url"`
	Size   int64  `json:"size"`
	Parts  int    `json:"parts"`
	Done   []bool `json:"done"`
	M3U8   bool   `json:"m3u8"`
	Prefix int64  `json:"prefix"`
}

// loadState only accepts a state that describes this exact job; anything else starts
// from zero, because a half-applied resume is worse than a rerun.
func (j *HttpJob) loadState() {
	b, err := os.ReadFile(j.statePath())
	if err != nil {
		return
	}
	var s httpState
	if err := json.Unmarshal(b, &s); err != nil {
		j.logf("续传状态解析失败，从头开始: %v", err)
		return
	}
	if s.URL != j.URL || s.Size != j.total || s.Parts != len(j.Parts) || s.M3U8 != j.m3u8 || len(s.Done) != len(j.Parts) {
		j.logf("续传状态对不上（url/尺寸/块数变了），从头开始")
		return
	}
	if _, err := os.Stat(j.Tmp); err != nil {
		return
	}
	n := 0
	j.mu.Lock()
	for i, d := range s.Done {
		j.done[i] = d
		if d {
			n++
			j.completed += j.Parts[i].size
		}
	}
	for j.prefixIdx < len(j.Parts) && j.done[j.prefixIdx] {
		j.prefix += j.Parts[j.prefixIdx].size
		j.prefixIdx++
	}
	j.next.Store(int64(j.prefixIdx))
	j.resume = fmt.Sprintf("续传 %d/%d 块", n, len(j.Parts))
	j.mu.Unlock()
	j.logf("%s", j.resume)
}

func (j *HttpJob) saveState() {
	j.mu.Lock()
	s := httpState{URL: j.URL, Size: j.total, Parts: len(j.Parts), Done: append([]bool(nil), j.done...), M3U8: j.m3u8, Prefix: j.prefix}
	j.mu.Unlock()
	b, err := json.Marshal(s)
	if err != nil {
		return
	}
	_ = os.WriteFile(j.statePath(), b, 0o600)
}

func (j *HttpJob) take() (int, bool) {
	for {
		i := int(j.next.Add(1)) - 1
		if i >= len(j.Parts) {
			return 0, false
		}
		j.mu.Lock()
		already := j.done[i]
		j.mu.Unlock()
		if already {
			continue
		}
		return i, true
	}
}

// commit advances the contiguous prefix only when the lowest open part lands, which
// is the whole reason parts are allocated in order. A short block is an error rather
// than a size correction: every part after it is written at its own fixed offset, so
// "resizing" one would relabel the hole instead of filling it.
func (j *HttpJob) commit(i int, got int64) error {
	if got != j.Parts[i].size {
		return fmt.Errorf("块 %d 只到 %d/%d 字节", i, got, j.Parts[i].size)
	}
	j.mu.Lock()
	j.done[i] = true
	j.completed += j.Parts[i].size
	for j.prefixIdx < len(j.Parts) && j.done[j.prefixIdx] {
		j.prefix += j.Parts[j.prefixIdx].size
		j.prefixIdx++
	}
	j.complete = j.prefixIdx == len(j.Parts)
	j.mu.Unlock()
	return nil
}

func (j *HttpJob) addWritten(n int64) {
	j.mu.Lock()
	j.written += n
	j.mu.Unlock()
}

func (j *HttpJob) fetchPart(f *os.File, i int) error {
	j.live.Add(1)
	defer j.live.Add(-1)
	p := j.Parts[i]
	req, err := j.newReq(p.url)
	if err != nil {
		return err
	}
	if !j.m3u8 && p.size > 0 && !(p.start == 0 && p.size == j.total) {
		req.Header.Set("Range", fmt.Sprintf("bytes=%d-%d", p.start, p.start+p.size-1))
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("块 %d: %w", i, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 && resp.StatusCode != 206 {
		return fmt.Errorf("块 %d http %d", i, resp.StatusCode)
	}
	buf := make([]byte, 256*1024)
	var off int64
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := f.WriteAt(buf[:n], p.start+off); werr != nil {
				return werr
			}
			off += int64(n)
			j.addWritten(int64(n))
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			return fmt.Errorf("块 %d 读断: %w", i, rerr)
		}
	}
	return j.commit(i, off)
}

func (j *HttpJob) run(conns int, quit <-chan struct{}) error {
	if conns <= 0 {
		conns = 8
	}
	if err := os.MkdirAll(filepath.Dir(j.Tmp), 0o755); err != nil {
		return err
	}
	f, err := os.OpenFile(j.Tmp, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	if err := f.Truncate(j.total); err != nil {
		return err
	}
	j.loadState()

	var wg sync.WaitGroup
	errc := make(chan error, 1)
	for w := 0; w < conns; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case <-quit:
					return
				default:
				}
				i, ok := j.take()
				if !ok {
					return
				}
				if err := j.fetchPart(f, i); err != nil {
					select {
					case errc <- err:
					default:
					}
					return
				}
				if i%16 == 0 || j.isComplete() {
					j.saveState()
				}
			}
		}()
	}

	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	var last int64
	for {
		select {
		case <-done:
			// A worker's error closes done too; without draining here the loop could
			// return nil and leave a half-filled job looking like a finished one.
			select {
			case err := <-errc:
				j.saveState()
				return err
			default:
			}
			j.saveState()
			return nil
		case err := <-errc:
			// Let the remaining workers notice nothing will mark their parts done and
			// exit on their own; quitting mid-request corrupts nothing, the bitmap is
			// the only truth written.
			j.saveState()
			return err
		case <-tick.C:
			j.mu.Lock()
			j.rate = float64(j.written - last)
			last = j.written
			j.mu.Unlock()
		case <-quit:
			j.saveState()
			return nil
		}
	}
}

// Finish renames the filled file into place and drops the state: a leftover state
// beside a completed file is a resume that can never be trusted again. The row only
// reads complete after the rename, because the shell turns "complete" into a path
// and a path that is still ending in .part is a row that cannot be played.
func (j *HttpJob) Finish() error {
	if !j.isComplete() {
		return fmt.Errorf("还没下完")
	}
	if err := os.Rename(j.Tmp, j.Out); err != nil {
		return err
	}
	_ = os.Remove(j.statePath())
	j.mu.Lock()
	j.finalized = true
	j.mu.Unlock()
	return nil
}

func (j *HttpJob) isComplete() bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.complete
}

// waitViews gives in-flight readers a chance to finish their body once the last part
// has landed: with the file complete the serve loops drain in milliseconds, so the
// cap is only there for a paused player holding the connection open.
func (j *HttpJob) waitViews(d time.Duration) {
	deadline := time.Now().Add(d)
	for j.views.Load() > 0 && time.Now().Before(deadline) {
		time.Sleep(50 * time.Millisecond)
	}
}

func (j *HttpJob) prefixBytes() int64 {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.prefix
}

func (j *HttpJob) totalBytes() int64 {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.total
}

func (j *HttpJob) status() Status {
	j.mu.Lock()
	defer j.mu.Unlock()
	st := Status{
		Name: j.name, Total: j.total, Completed: j.completed, Missing: j.total - j.completed,
		Rate: j.rate, NumPieces: len(j.Parts), Complete: j.finalized, Want: "all",
		DataDir: filepath.Dir(j.Out), ElapsedMs: time.Since(j.started).Milliseconds(),
		Resume: j.resume,
	}
	for _, d := range j.done {
		if d {
			st.PiecesDone++
		}
	}
	st.Peers = int(j.live.Load())
	st.ActivePeers = st.Peers
	if st.Total > 0 {
		st.Pct = float64(st.Completed) / float64(st.Total) * 100
	}
	for i, p := range j.Parts {
		c := p.size
		if !j.done[i] {
			c = 0
		}
		f := FileStatus{Index: i, Path: strconv.Itoa(i), Length: p.size, Completed: c}
		if p.size > 0 {
			f.Pct = float64(c) / float64(p.size) * 100
		}
		st.Files = append(st.Files, f)
	}
	return st
}

// serve answers /file/0 out of the file itself, waiting on the prefix rather than
// returning a hole. mpv's reads are sequential during playback, so an arriving byte
// is usually the one it wants next.
func (j *HttpJob) serve(w http.ResponseWriter, r *http.Request) {
	j.views.Add(1)
	defer j.views.Add(-1)
	// After Finish the bytes live at Out; during the rename window neither path is
	// guaranteed, so try both rather than answering 404 for a file that is right there.
	f, err := openShared(j.Tmp)
	if err != nil {
		f, err = openShared(j.Out)
	}
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	defer f.Close()
	total := j.totalBytes()
	w.Header().Set("Accept-Ranges", "bytes")
	w.Header().Set("Content-Type", "application/octet-stream")
	start, end := int64(0), total-1
	if rng := r.Header.Get("Range"); rng != "" {
		s, e, ok := parseRange(rng, total)
		if !ok {
			w.Header().Set("Content-Range", fmt.Sprintf("bytes */%d", total))
			http.Error(w, "unsatisfiable range", http.StatusRequestedRangeNotSatisfiable)
			return
		}
		start, end = int64(s), int64(e)
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, end, total))
		w.WriteHeader(http.StatusPartialContent)
	}
	if r.Method == "HEAD" {
		// A probe must answer now: waiting on the prefix here would make every
		// HEAD look like a stalled server.
		if r.Header.Get("Range") == "" {
			w.Header().Set("Content-Length", strconv.FormatInt(total, 10))
		}
		return
	}
	buf := make([]byte, 256*1024)
	deadline := time.Now().Add(120 * time.Second)
	for pos := start; pos <= end; {
		want := min(int64(len(buf)), end-pos+1)
		avail := j.prefixBytes() - pos
		if j.isComplete() {
			avail = end - pos + 1
		}
		if avail <= 0 {
			if time.Now().After(deadline) {
				return
			}
			time.Sleep(100 * time.Millisecond)
			continue
		}
		n := int(min(want, avail))
		if _, err := f.ReadAt(buf[:n], pos); err != nil {
			return
		}
		if _, err := w.Write(buf[:n]); err != nil {
			return
		}
		if fl, ok := w.(http.Flusher); ok {
			fl.Flush()
		}
		pos += int64(n)
	}
}

func serveHTTPJob(j *HttpJob, addr string, quit chan struct{}) (*Server, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /status", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(j.status())
	})
	mux.HandleFunc("GET /file/{index}", func(w http.ResponseWriter, r *http.Request) { j.serve(w, r) })
	// The shell drives both engines through the same two verbs; an HTTP job has no
	// stream/all distinction of its own, so /want only has to answer instead of 404.
	mux.HandleFunc("POST /want/{mode}", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintln(w, "ok")
	})
	srv := &http.Server{ReadHeaderTimeout: 10 * time.Second, Handler: mux}
	s := &Server{Stopped: make(chan struct{}), Quit: quit, srv: srv}
	mux.HandleFunc("POST /quit", func(w http.ResponseWriter, r *http.Request) {
		s.quitOne.Do(func() {
			close(quit)
			fmt.Println("QUIT")
		})
		fmt.Fprintln(w, "quitting")
	})
	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		st := j.status()
		fmt.Fprintf(w, "%s\n", st.Name)
		fmt.Fprintf(w, "total=%d completed=%d pct=%.2f rate=%.2fMB/s parts=%d/%d\n",
			st.Total, st.Completed, st.Pct, st.Rate/1024/1024, st.PiecesDone, st.NumPieces)
	})
	go func() { _ = srv.Serve(ln); close(s.Stopped) }()
	s.URL = "http://" + ln.Addr().String()
	return s, nil
}
