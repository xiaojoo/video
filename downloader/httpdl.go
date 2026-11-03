package main

import (
	"bufio"
	"encoding/json"
	"errors"
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
	// lanes is how many workers may take a block at once. It starts at what the shell asked
	// for and only goes down: a host that refuses the eleventh road is telling us its ceiling,
	// and the way to find the number is to keep losing one road until the refusals stop.
	lanes atomic.Int32
	// wanted is the concurrency the shell asked for, kept so the floor can be a fraction of it
	// rather than a magic number that outlives its measurement.
	wanted int32
	// back holds blocks handed back by a refused worker, so a refusal costs a detour rather
	// than the whole job.
	back []int
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
	if p := localFileOf(raw); p != "" {
		base := filepath.Base(p)
		if base == "." || base == "/" || base == "" {
			return "download"
		}
		return base
	}
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

// localFileOf recognises a source that is a file on this machine rather than a url.
//
// A 百度 share's playback table can only be minted by the site's own tab: measured, the same
// /share/streaming url answers a 10-segment preview to a client that is not a browser and
// 962 segments to the page. So the table reaches the engine as a path on disk, and only the
// segments it lists cross the network - those are absolute https and need no cookie.
func localFileOf(raw string) string {
	if strings.HasPrefix(strings.ToLower(raw), "file://") {
		u, err := url.Parse(raw)
		if err != nil {
			return ""
		}
		p := u.Path
		if len(p) > 2 && p[0] == '/' && p[2] == ':' {
			p = p[1:] // file:///C:/x wants C:/x, not /C:/x
		}
		return filepath.FromSlash(p)
	}
	if len(raw) >= 3 && raw[1] == ':' && (raw[2] == '\\' || raw[2] == '/') {
		return raw
	}
	return ""
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
	if p := localFileOf(raw); p != "" {
		b, rerr := os.ReadFile(p)
		if rerr != nil {
			return 0, false, "", nil, rerr
		}
		// The content-type is what the caller's playlist branch keys off, and a file on
		// disk has none; ranges are meaningless for a table we are already holding.
		return int64(len(b)), false, "application/vnd.apple.mpegurl", b, nil
	}
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
// numbers instead of a running guess.
//
// The one-byte ranged GET comes FIRST, not as a fallback, because a HEAD's Content-Length is
// only a claim about the resource and some hosts make the wrong one: measured on a 百度
// playlist, every segment answered HEAD with 26,677,200 - the same number, which is the
// `ts_size` query parameter the url carries - while the same segment's `bytes=0-0` answer
// carried `Content-Range: bytes 0-0/694284`, matching the 694,284 bytes a real GET returns.
// Trusting the HEAD made the whole film 962 × 26.7 MB = 30,362,863,108 bytes, and would have
// written every segment at a 26 MB stride into a sparse file.
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
			req.Header.Set("Range", "bytes=0-0")
			resp, err := http.DefaultClient.Do(req)
			if err == nil {
				resp.Body.Close()
				if cr := resp.Header.Get("Content-Range"); cr != "" {
					if _, after, ok := strings.Cut(cr, "/"); ok {
						sizes[i], _ = strconv.ParseInt(strings.TrimSpace(after), 10, 64)
					}
				}
				if sizes[i] <= 0 && resp.StatusCode == http.StatusOK {
					sizes[i] = resp.ContentLength
				}
			}
			if sizes[i] > 0 {
				return
			}
			req, err = j.newReq(s)
			if err != nil {
				return
			}
			req.Method = "HEAD"
			resp, err = http.DefaultClient.Do(req)
			if err == nil {
				resp.Body.Close()
				if resp.StatusCode == http.StatusOK && resp.ContentLength > 0 {
					sizes[i] = resp.ContentLength
					return
				}
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
	// A handed-back block goes first: it was refused for how many roads were open, not for
	// what it asked for, and leaving it at the tail would make the worker that just lost a
	// race wait behind everything else.
	j.mu.Lock()
	if n := len(j.back); n > 0 {
		i := j.back[n-1]
		j.back = j.back[:n-1]
		j.mu.Unlock()
		if !j.done[i] {
			return i, true
		}
	} else {
		j.mu.Unlock()
	}
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

// requeue gives a block back to whoever is next free.
func (j *HttpJob) requeue(i int) {
	j.mu.Lock()
	j.back = append(j.back, i)
	j.mu.Unlock()
}

// lower takes one road away and reports how many are left, down to the floor the job set.
// The floor is not 1 because that is not where the refusals stop: measured on one 迅雷 address,
// 12 roads walk down through 11/10/9/8/7/6 and keep being refused all the way to 2, while 4
// roads answer clean for a whole minute. Stopping the walk at 4 leaves the job at the number
// this kind of host actually serves, and a host that really needs fewer still gets refused
// loudly rather than silently crawling.
func (j *HttpJob) lower() int32 {
	for {
		n := j.lanes.Load()
		if n <= j.floor() {
			return n
		}
		if j.lanes.CompareAndSwap(n, n-1) {
			return n - 1
		}
	}
}

// floor is the smallest number of roads a job will walk down to: the requested concurrency
// when that is already small, otherwise four.
func (j *HttpJob) floor() int32 {
	if j.wanted <= 4 {
		return j.wanted
	}
	return 4
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
		return &httpRefused{block: i, code: resp.StatusCode}
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

// A refusal is not always a dead address. Measured on 迅雷's CDN with one signed url: 12
// simultaneous range requests answer 10×206 and 2×503, while 1, 4 and 8 come back clean - the
// edge caps how many connections one address will serve at a time. Until this type existed any
// block's error ended the whole job, so a 迅雷 row sat at 「块 10 http 503」 with a byte
// never written. The code is what the worker loop's one retry decision reads.
type httpRefused struct {
	block int
	code  int
}

func (e *httpRefused) Error() string { return fmt.Sprintf("块 %d http %d", e.block, e.code) }

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
	// The shell asked for this many roads; the host gets the final say, one refusal at a time.
	//
	// There used to be a hardcoded 4 here for *.xunlei.com, from a measurement that read 8 and 12
	// roads as refusing immediately. Measured again 2026-11-03 on a live 迅雷 address: one road
	// delivers 0.11 MB/s and an 8,360,585-byte range lands complete in 73 s with no truncation -
	// so a job at 4 roads is capped at ~0.44 MB/s by the clamp, not by the host. The clamp is gone
	// and the adaptive lane below is the whole answer: open at what he set, lose a road per
	// refusal, floor at min(4, asked). If a host really caps roads the row settles there on its
	// own and says so in the log.
	j.wanted = int32(conns)
	j.lanes.Store(int32(conns))

	var wg sync.WaitGroup
	errc := make(chan error, 1)
	for w := 0; w < conns; w++ {
		wg.Add(1)
		go func(me int) {
			defer wg.Done()
			streak := 0
			for {
				select {
				case <-quit:
					return
				default:
				}
				if int32(me) >= j.lanes.Load() {
					// This worker is the price the last refusal was paid with. Exiting here
					// is what actually lowers the concurrency: nothing else closes the socket.
					return
				}
				i, ok := j.take()
				if !ok {
					return
				}
				err := j.fetchPart(f, i)
				if err == nil {
					streak = 0
					if i%16 == 0 || j.isComplete() {
						j.saveState()
					}
					continue
				}
				// One answer ends the job on the spot: a 4xx that is not the host's "too many
				// at once". That is the 412-with-a-262-byte-page the cookie story reads, and a
				// row that retried it would look like a drive that is merely slow.
				var ref *httpRefused
				if errors.As(err, &ref) && ref.code != http.StatusTooManyRequests &&
					ref.code != http.StatusRequestedRangeNotSatisfiable && ref.code < 500 {
					select {
					case errc <- err:
					default:
					}
					return
				}
				// Everything else is the host not serving this many roads right now, in one of
				// three voices. Measured on 迅雷's CDN with one signed url: 12 roads answer
				// 10×206 + 2×503 and 16 add a 416; a road closes mid-carry so the block lands
				// short (块 14 只到 4,568,059/8,374,053 字节); or the read breaks
				// outright (块 5 读断). Those last two used to get six quick retries and then
				// kill the job, which is how a row that had already landed 117 MB still died.
				// So: hand the block back, pay a lane while there are lanes to pay, and wait
				// the cooldown out at the floor - measured, one address that starts refusing
				// keeps refusing for tens of seconds even at four roads, then serves again.
				// 416 is a different animal from a refusal. Measured on 迅雷's free 720P
				// address for a 461,561,620-byte file: it answers 206 for bytes 0-23,619,000 and
				// for the last ~21 MB, and 416 for everything between - at 0 s, at 45 s and at
				// 90 s. That is the transcode being prepared only where a player has looked, not
				// a cap on roads, so it costs no lane and it does not count toward the death
				// streak: the block is asked again on a slow clock and the rest of the job keeps
				// taking what the host is serving.
				if errors.As(err, &ref) && ref.code == http.StatusRequestedRangeNotSatisfiable {
					j.requeue(i)
					time.Sleep(20 * time.Second)
					continue
				}
				j.requeue(i)
				if j.lanes.Load() > j.floor() {
					if j.logf != nil {
						j.logf("并发降到 %d（%s）", j.lower(), err)
					}
					time.Sleep(300 * time.Millisecond)
					continue
				}
				// Forty attempts at the floor is close to a minute of nothing but errors: that
				// is not a ceiling any more, it is the host saying no to the job, and a row
				// that never moves is worse than a row that says so.
				if streak++; streak >= 40 {
					select {
					case errc <- err:
					default:
					}
					return
				}
				time.Sleep(1500 * time.Millisecond)
			}
		}(w)
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
		// The name of the file this job is actually writing, not a re-derivation of the
		// url: the shell copies this field over the row's name, and a 夸克 url's last path
		// segment is a 40-character object id - the row then points at a path that was
		// never written while the real file sits beside it under the name it was given.
		Name: filepath.Base(j.Out), Total: j.total, Completed: j.completed, Missing: j.total - j.completed,
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
