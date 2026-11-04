package main

import (
	"encoding/binary"
	"os"
)

// The length a MPEG-TS carries in its own timestamps.
//
// 迅雷 quotes none for the files it will hand over on the free tier - measured: the field is
// there and it is the string "0" - so the shell had nothing but mpv's estimate, which is
// declared size over the bitrate it probed off the first megabytes. On 凡人修仙传 年番4 that
// came out 7:40 against a film that runs 28:30, i.e. 3.7x short, and the bar and the clamp
// both inherit the lie.
//
// The same free tier does serve the last part, so the newest PTS in the file is the film's
// own end time. This reads two 2 MiB windows - the head, to know where the clock started -
// and takes the span. It runs once per job, behind a sync.Once, because /status is polled
// every second and the answer cannot change while the file is the same length.

const tsPacket = 188
const tsWindow = 2 << 20

type tsScan struct {
	min, max uint64
	packets  int
	withPts  int
}

func tsPackets(b []byte) tsScan {
	var s tsScan
	first := true
	for i := 0; i+tsPacket <= len(b); i += tsPacket {
		p := b[i : i+tsPacket]
		if p[0] != 0x47 {
			continue
		}
		s.packets++
		if p[1]&0x40 == 0 { // no payload_unit_start_indicator: no PES header begins here
			continue
		}
		off := 4
		if p[3]&0x20 != 0 { // an adaptation field sits in front of the payload
			afl := int(p[4])
			if afl < 1 || 5+afl > len(p) {
				continue
			}
			off = 5 + afl
		}
		if off+14 > len(p) || p[off] != 0 || p[off+1] != 0 || p[off+2] != 1 {
			continue
		}
		if p[off+7]&0xC0 == 0 { // PTS_DTS_flags: 01 = PTS, 11 = PTS and DTS
			continue
		}
		q := p[off+9 : off+14]
		pts := uint64(q[0]>>1&0x07) << 30
		pts |= uint64(binary.BigEndian.Uint16(q[1:3])) >> 1 << 15
		pts |= uint64(q[3] >> 1)
		if first {
			s.min, s.max, first = pts, pts, false
		}
		if pts < s.min {
			s.min = pts
		}
		if pts > s.max {
			s.max = pts
		}
		s.withPts++
	}
	return s
}

func tsWindowAt(f *os.File, off int64, n int) []byte {
	if _, err := f.Seek(off, 0); err != nil {
		return nil
	}
	b := make([]byte, n)
	read := 0
	for read < n {
		k, err := f.Read(b[read:])
		read += k
		if err != nil || k == 0 {
			break
		}
	}
	return b[:read]
}

// tsResync steps to three packet starts that all carry the sync byte; a window cut out of
// the middle of a file does not begin on a packet boundary.
func tsResync(b []byte) []byte {
	for i := 0; i+3*tsPacket <= len(b); i++ {
		if b[i] == 0x47 && b[i+tsPacket] == 0x47 && b[i+2*tsPacket] == 0x47 {
			return b[i:]
		}
	}
	return b
}

// tsDurationOf returns the film's length in seconds, or 0 when the file is not a TS, is
// shorter than the two windows, or carries no timestamps.
func tsDurationOf(path string) float64 {
	f, err := os.Open(path) // read only; a job that is still filling owns the file
	if err != nil {
		return 0
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || st.Size() < 4*tsWindow {
		return 0
	}
	head := tsPackets(tsResync(tsWindowAt(f, 0, tsWindow)))
	tail := tsPackets(tsResync(tsWindowAt(f, st.Size()-tsWindow, tsWindow)))
	// Both windows have to hold timestamps: a job whose tail has not landed yet reads zeros
	// there, and an mp4 or a matroska is not this file shape at all.
	if head.withPts == 0 || tail.withPts == 0 {
		return 0
	}
	secs := float64(tail.max-head.min) / 90000
	if secs <= 0 || secs > 12*3600 {
		return 0
	}
	return secs
}

// tsDuration keeps the answer per job. It is only worth opening the file once the last part
// has actually landed, which is also the moment the number first becomes available; before
// that the tail window is zeros and the answer stays 0.
func (j *HttpJob) tsDuration() float64 {
	if j.durSec > 0 {
		return j.durSec
	}
	if len(j.done) == 0 || !j.done[len(j.done)-1] {
		return 0
	}
	path := j.Tmp
	if j.finalized {
		path = j.Out
	}
	j.durSec = tsDurationOf(path)
	return j.durSec
}
