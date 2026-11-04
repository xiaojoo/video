package main

import (
	"context"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	g "github.com/anacrolix/generics"
	"github.com/anacrolix/log"
	"github.com/anacrolix/torrent"
	"github.com/anacrolix/torrent/metainfo"
	"github.com/anacrolix/torrent/storage"
	"golang.org/x/time/rate"
)

// Engine owns one torrent client and the one job this process exists for.
//
// Resume state lives in three separate places, and the split is what makes the
// behaviour measurable: the .torrent bytes (so a restart does not have to
// re-discover metadata over DHT), the piece-completion store (so a restart does
// not have to re-hash the files), and the data files themselves.
type Engine struct {
	cl *torrent.Client
	t  *torrent.Torrent

	dataDir  string
	stateDir string
	metaPath string
	want     string

	mu        sync.Mutex
	startedAt time.Time
	lastBytes int64
	lastAt    time.Time
	rate      float64
	resume    string
	resumeMs  int64
	closed    bool
}

type FileStatus struct {
	Index     int     `json:"index"`
	Path      string  `json:"path"`
	Length    int64   `json:"length"`
	Completed int64   `json:"completed"`
	Pct       float64 `json:"pct"`
}

// PieceRun is the engine's own answer to "which parts do you actually have", in
// runs. An offset is not evidence of being downloaded: pieces arrive in the
// swarm's order, so a measurement that assumes sequential fill will label its own
// numbers wrong.
type PieceRun struct {
	Length   int  `json:"len"`
	Known    bool `json:"known"`
	Complete bool `json:"complete"`
	Partial  bool `json:"partial"`
	Hashing  bool `json:"hashing"`
	Priority int  `json:"prio"`
}

type Status struct {
	Hash      string `json:"hash"`
	Name      string `json:"name"`
	Total     int64  `json:"total"`
	Completed int64  `json:"completed"`
	// The contiguous prefix: the byte the reader can reach without meeting a hole. On a
	// free-tier drive the tail lands beside a middle that is refused forever, so completed
	// is larger than what is playable and only this number says where the film stops.
	Prefix  int64   `json:"prefix"`
	Missing int64   `json:"missing"`
	Pct     float64 `json:"pct"`
	// The length the file itself carries, when the engine can read it off the bytes it has.
	// 迅雷's own field for this is the string "0", so this is the only number that says how
	// long the film is on a drive that will not hand over the middle.
	Duration    float64      `json:"duration"`
	Rate        float64      `json:"rate"`
	Peers       int          `json:"peers"`
	ActivePeers int          `json:"activePeers"`
	Seeders     int          `json:"seeders"`
	NumPieces   int          `json:"numPieces"`
	PiecesDone  int          `json:"piecesDone"`
	Complete    bool         `json:"complete"`
	Seeding     bool         `json:"seeding"`
	Want        string       `json:"want"`
	Resume      string       `json:"resume"`
	DataDir     string       `json:"dataDir"`
	ResumeMs    int64        `json:"resumeMs"`
	ElapsedMs   int64        `json:"elapsedMs"`
	Files       []FileStatus `json:"files"`
}

// silence mutes the engine's own log, which is a firehose at Info level and shares
// stderr with the status lines this process exists to emit.
func silence(cfg *torrent.ClientConfig) {
	lg := log.Default
	lg.SetHandlers(log.DiscardHandler)
	cfg.Logger = lg
}

func newEngine(dataDir, stateDir string, limitMB float64, seed, debug, isolated bool) (*Engine, error) {
	if err := os.MkdirAll(filepath.Join(stateDir, "meta"), 0o755); err != nil {
		return nil, err
	}
	// The download lands where the user asked for it; the engine's own bookkeeping -
	// the .torrent bytes and the piece-completion store - lives beside the rest of
	// the app's state, so a save folder holds one file and not a folder of engine
	// droppings.
	completion, err := storage.NewDefaultPieceCompletionForDir(stateDir)
	if err != nil {
		return nil, fmt.Errorf("piece completion store: %w", err)
	}

	cfg := torrent.NewDefaultClientConfig()
	cfg.DataDir = dataDir
	cfg.DefaultStorage = storage.NewFileOpts(storage.NewFileClientOpts{
		ClientBaseDir:   dataDir,
		PieceCompletion: completion,
		// Part files are on by default, and the storage layer's own doc says what
		// that does to resume: with them enabled, completion is inferred from the
		// file names at startup and the rest is kept in memory. Resume across process
		// restarts is the whole point here, so the pieces go into the file the user
		// will end up keeping, and their state goes into the store.
		UsePartFiles: g.Some(false),
	})
	cfg.Seed = seed
	cfg.Debug = debug
	if !debug {
		silence(cfg)
	}
	// An ephemeral sidecar has no business opening ports on the LAN: UPnP through
	// the user's router is visible behaviour they never asked for.
	cfg.NoDefaultPortForwarding = true
	// The library's own default listen port is a fixed 42069, so only one of these
	// processes can be alive at a time: the second job died at once with "first listen:
	// bind: Only one usage of each socket address", and adding a second magnet looked
	// like a download that never started. Let the OS pick.
	cfg.ListenPort = 0
	cfg.DisablePEX = true
	cfg.EstablishedConnsPerTorrent = 50
	cfg.HalfOpenConnsPerTorrent = 25
	if isolated {
		// A measurement run that discovers peers only through -peers, so the rate
		// and the piece order are reproducible instead of depending on the swarm.
		cfg.NoDHT = true
		cfg.DisableTrackers = true
		cfg.DisableUTP = true
	}
	if limitMB > 0 {
		burst := int(limitMB * 1024 * 1024)
		// The limiter counts bytes and its burst has to hold a whole chunk (16 KiB)
		// plus the peer read buffer, so even a tight cap needs a floor here or
		// handshaking stalls.
		if burst < 1<<20 {
			burst = 1 << 20
		}
		cfg.DownloadRateLimiter = rate.NewLimiter(rate.Limit(limitMB*1024*1024), burst)
	}

	cl, err := torrent.NewClient(cfg)
	if err != nil {
		return nil, err
	}
	return &Engine{cl: cl, dataDir: dataDir, stateDir: stateDir}, nil
}

// A bare `magnet:?xt=urn:btih:…` leaves exactly one way to find anyone: the DHT crawl.
// These sites hand out bare magnets, PEX is off, and no port is mapped, so the crawl is
// the whole story - and measured on this box against three torrents from the site the
// user pointed at, it never paid off: no metadata in 45 s, three times out of three.
// The same three hashes with four public trackers named reached 27 / 13 / 25 peers and
// moved 13.5 MB / 38.4 MB / 8.5 MB. So a magnet that names no tracker gets this standing
// set appended; one that already names trackers keeps precisely those.
var fallbackTrackers = []string{
	"udp://tracker.opentrackr.org:1337/announce",
	"udp://tracker.torrent.eu.org:451/announce",
	"udp://open.stealth.si:80/announce",
	"udp://exodus.desync.com:6969/announce",
}

func withTrackers(src string) string {
	if !strings.HasPrefix(src, "magnet:") || strings.Contains(src, "tr=") {
		return src
	}
	for _, tr := range fallbackTrackers {
		src += "&tr=" + url.QueryEscape(tr)
	}
	return src
}

// start resolves the source into a live torrent, restores whatever resume state
// is already on disk, and then sets what this job wants: every piece, or only the
// regions a reader turns up.
func (e *Engine) start(src, peers, want string) error {
	var (
		t   *torrent.Torrent
		err error
	)
	if strings.HasPrefix(src, "magnet:") {
		t, err = e.cl.AddMagnet(withTrackers(src))
	} else {
		t, err = e.cl.AddTorrentFromFile(src)
	}
	if err != nil {
		return err
	}
	e.t = t
	e.metaPath = filepath.Join(e.stateDir, "meta", t.InfoHash().HexString()+".torrent")
	e.startedAt = time.Now()
	e.lastAt = e.startedAt
	// Peers before metadata: with a magnet the info section is something the swarm
	// has to hand over, so dialling the known seeds is the only way past WAITMETA.
	if n := e.addPeers(peers); n > 0 {
		fmt.Printf("PEERS %d\n", n)
	}

	// Metadata before anything else: without the piece map there is nothing to
	// resume into. A saved .torrent is what turns a magnet restart into an
	// instant one, and SetInfoBytes is only legal while the torrent has no info.
	var hadMeta bool
	select {
	case <-t.GotInfo():
		hadMeta = true
	default:
	}
	if !hadMeta {
		if _, statErr := os.Stat(e.metaPath); statErr == nil {
			// What is on disk is a whole .torrent; SetInfoBytes wants only the info
			// dictionary out of it. Passing the file itself decodes to an info with no
			// name and no length, and the storage layer then tries to open the torrent
			// directory as a zero-length file.
			mi, err := metainfo.LoadFromFile(e.metaPath)
			if err != nil {
				return fmt.Errorf("read %s: %w", filepath.Base(e.metaPath), err)
			}
			// A meta file describing a different infohash would start the wrong download
			// in silence, so it is checked before any of it is trusted.
			if got := mi.HashInfoBytes(); got != t.InfoHash() {
				return fmt.Errorf("saved torrent describes %s, not %s",
					got.HexString(), t.InfoHash().HexString())
			}
			if err := t.SetInfoBytes(mi.InfoBytes); err != nil {
				return fmt.Errorf("restore metadata: %w", err)
			}
			hadMeta = true
		} else {
			// Metadata has to come off the swarm before there is a piece map, and on a
			// thin torrent that is the slow part. Say so instead of looking hung.
			fmt.Printf("WAITMETA %s\n", t.InfoHash().HexString())
			os.Stdout.Sync()
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	select {
	case <-t.GotInfo():
	case <-ctx.Done():
		return fmt.Errorf("no metadata within 120s; peers known=%d, no seeders reachable?", t.Stats().TotalPeers)
	}

	if err := e.saveMeta(); err != nil {
		return err
	}
	e.verify()

	// "all" wants every piece, which is what a download manager does. "stream" asks
	// for nothing and lets each reader pull only the region it is about to read -
	// that is what makes watching-while-downloading cost seconds instead of the rest
	// of the file, and it is the only shape that separates "the read brought its own
	// bytes" from "that piece was queued anyway".
	e.want = want
	switch want {
	case "stream":
		t.AllowDataDownload()
		// Nothing is asked for yet: a reader queues the pieces it is about to read.
		// Printed as a baseline so "stream mode pulled nothing" cannot hide here.
		fmt.Printf("STREAM %d pieces wanted at start\n", wantedPieces(t))
	default:
		t.DownloadAll()
		t.AllowDataDownload()
	}
	return nil
}

func wantedPieces(t *torrent.Torrent) int {
	n := 0
	for _, r := range t.PieceStateRuns() {
		// Priority 0 means "not wanted", and it is the zero value, so anything above it
		// is a piece the engine has been asked to fetch.
		if r.Priority > 0 || r.Complete || r.Partial {
			n += r.Length
		}
	}
	return n
}

// verify re-hashes the files on disk when the completion store comes up with
// nothing. Whether a re-hash is owed is a question about the files, not about
// where the metadata came from, so it is asked of the engine instead of inferred.
func (e *Engine) verify() {
	t0 := time.Now()
	known := e.t.Stats().PiecesComplete
	mode := "store trusted"
	if known == 0 {
		mode = "re-hashed"
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
		defer cancel()
		if err := e.t.VerifyDataContext(ctx); err != nil {
			e.setResume(fmt.Sprintf("verify failed: %v", err), 0)
			return
		}
	}
	ms := time.Since(t0).Milliseconds()
	e.setResume(fmt.Sprintf("%d/%d pieces complete after %s (store claimed %d up front), %d bytes",
		e.t.Stats().PiecesComplete, e.t.NumPieces(), mode, known, e.t.BytesCompleted()), ms)
}

func (e *Engine) setResume(s string, ms int64) {
	e.mu.Lock()
	e.resume, e.resumeMs = s, ms
	e.mu.Unlock()
}

func (e *Engine) saveMeta() error {
	mi := e.t.Metainfo()
	f, err := os.Create(e.metaPath)
	if err != nil {
		return err
	}
	defer f.Close()
	return mi.Write(f)
}

func (e *Engine) status() Status {
	e.mu.Lock()
	st := Status{Want: e.want, Resume: e.resume, ResumeMs: e.resumeMs, DataDir: e.dataDir,
		ElapsedMs: time.Since(e.startedAt).Milliseconds(), Rate: e.rate}
	e.mu.Unlock()

	t := e.t
	if t == nil {
		return st
	}
	st.Hash = t.InfoHash().HexString()
	st.Complete = t.Complete().Bool()
	st.Seeding = t.Seeding()
	st.Missing = t.BytesMissing()
	st.Completed = t.BytesCompleted()
	st.NumPieces = t.NumPieces()
	if info := t.Info(); info != nil {
		st.Name = info.Name
		st.Total = info.Length
	}
	for i, f := range t.Files() {
		st.Files = append(st.Files, FileStatus{
			Index: i, Path: f.DisplayPath(), Length: f.Length(), Completed: f.BytesCompleted(),
		})
	}
	s := t.Stats()
	st.Peers = s.TotalPeers
	st.ActivePeers = s.ActivePeers
	st.Seeders = s.ConnectedSeeders
	st.PiecesDone = s.PiecesComplete

	if st.Total > 0 {
		st.Pct = 100 * float64(st.Completed) / float64(st.Total)
	}
	for i := range st.Files {
		if st.Files[i].Length > 0 {
			st.Files[i].Pct = 100 * float64(st.Files[i].Completed) / float64(st.Files[i].Length)
		}
	}

	e.mu.Lock()
	dt := time.Since(e.lastAt).Seconds()
	if dt > 0 {
		e.rate = float64(st.Completed-e.lastBytes) / dt
	}
	e.lastBytes = st.Completed
	e.lastAt = time.Now()
	e.mu.Unlock()
	return st
}

func (e *Engine) addPeers(spec string) int {
	if e.t == nil || spec == "" {
		return 0
	}
	return e.t.AddPeers(parsePeers(spec))
}

func (e *Engine) pieceRuns() []PieceRun {
	if e.t == nil {
		return nil
	}
	runs := e.t.PieceStateRuns()
	out := make([]PieceRun, 0, len(runs))
	for _, r := range runs {
		out = append(out, PieceRun{
			Length:   r.Length,
			Known:    r.Ok,
			Complete: r.Complete,
			Partial:  r.Partial,
			Hashing:  r.Hashing || r.QueuedForHash,
			Priority: int(r.Priority),
		})
	}
	return out
}

// setWant switches what the job asks the swarm for while it is running.
//
// "stream" cancels the per-piece want, which by the engine's own comment leaves the
// read cursor's interest alone: reads keep pulling their own bytes. "all" wants
// every piece. The return is how many pieces are wanted afterwards, which is the
// only way the caller can tell the switch actually did something.
func (e *Engine) setWant(mode string) (int, error) {
	if e.t == nil {
		return 0, fmt.Errorf("no torrent")
	}
	switch mode {
	case "stream":
		e.t.CancelPieces(0, e.t.NumPieces())
	case "all":
		e.t.DownloadAll()
	default:
		return 0, fmt.Errorf("unknown want %q", mode)
	}
	e.mu.Lock()
	e.want = mode
	e.mu.Unlock()
	return wantedPieces(e.t), nil
}

func (e *Engine) close() {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return
	}
	e.closed = true
	e.mu.Unlock()
	// The client drops every torrent it owns on Close, and that is the order that
	// lets the storage finish flushing; a separate Drop would race it.
	for _, err := range e.cl.Close() {
		fmt.Fprintf(os.Stderr, "close: %v\n", err)
	}
}
