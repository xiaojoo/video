package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	g "github.com/anacrolix/generics"
	"github.com/anacrolix/torrent"
	"github.com/anacrolix/torrent/bencode"
	"github.com/anacrolix/torrent/metainfo"
	"github.com/anacrolix/torrent/storage"
)

// Seed mode exists for measurement. A swarm whose only peer is a second process
// on 127.0.0.1 makes download rate, piece ordering and resume reproducible, which
// a real internet swarm does not - and it never announces anything to a public
// tracker.
func runSeed(path string, port int) error {
	abs, err := filepath.Abs(path)
	if err != nil {
		return err
	}
	st, err := os.Stat(abs)
	if err != nil {
		return err
	}
	if st.IsDir() {
		return fmt.Errorf("seed expects a single file, got a directory")
	}

	info := metainfo.Info{PieceLength: metainfo.ChoosePieceLength(st.Size())}
	if err := info.BuildFromFilePath(abs); err != nil {
		return err
	}
	mi := metainfo.MetaInfo{InfoBytes: bencode.MustMarshal(info)}
	ih := mi.HashInfoBytes()

	cfg := torrent.NewDefaultClientConfig()
	cfg.Seed = true
	cfg.NoDHT = true
	cfg.DisableTrackers = true
	cfg.NoDefaultPortForwarding = true
	cfg.DisablePEX = true
	cfg.DisableUTP = true // UDP only pays off on the internet, and an OS-picked UDP port can
	// be refused outright: measured here, udp4 :58908 came back WSAEACCES inside the
	// Hyper-V reserved range 58888-58987.
	cfg.ListenPort = port
	silence(cfg)
	cl, err := torrent.NewClient(cfg)
	if err != nil {
		return err
	}
	defer cl.Close()

	// The fixture does not change, so its piece set is worked out once at startup and
	// kept in memory. A bolt store here would only make two seeders on the same folder
	// fight over a lock file.
	completion := storage.NewMapPieceCompletion()
	t, _ := cl.AddTorrentOpt(torrent.AddTorrentOpts{
		InfoHash:  ih,
		InfoBytes: mi.InfoBytes,
		Storage: storage.NewFileOpts(storage.NewFileClientOpts{
			ClientBaseDir:   filepath.Dir(abs),
			PieceCompletion: completion,
			// Serve straight out of the fixture. The torrent directory is pinned to
			// the file's own folder and the file name is left to the default maker,
			// so the storage resolves to abs itself - no copy, and no path that a
			// rejoin can turn into base+base+name.
			TorrentDirMaker: func(string, *metainfo.Info, metainfo.Hash) string {
				return filepath.Dir(abs)
			},
			// Part files are on by default, and "on" means the seeder would rename the
			// file it is reading out from under the user.
			UsePartFiles: g.Some(false),
		}),
	})
	if err != nil {
		return err
	}
	// Nothing is marked complete until the file has been hashed; without this the
	// seeder has pieces to serve but does not know it.
	if err := t.VerifyData(); err != nil {
		return fmt.Errorf("verify seed data: %w", err)
	}
	t.AllowDataUpload()
	t.DisallowDataDownload()

	for _, a := range cl.ListenAddrs() {
		fmt.Printf("SEEDING %s on %s\n", a.Network(), a)
	}
	fmt.Printf("MAGNET %s\n", mi.Magnet(&ih, &info).String())
	os.Stdout.Sync()

	select {}
}

func parsePeers(spec string) []torrent.PeerInfo {
	var out []torrent.PeerInfo
	for _, a := range strings.Split(spec, ",") {
		a = strings.TrimSpace(a)
		if a == "" {
			continue
		}
		out = append(out, torrent.PeerInfo{Addr: stringAddr(a)})
	}
	return out
}

// PeerRemoteAddr only needs the net.Addr pair of methods; the client dials the
// unresolved string itself.
type stringAddr string

func (stringAddr) Network() string   { return "tcp" }
func (me stringAddr) String() string { return string(me) }
