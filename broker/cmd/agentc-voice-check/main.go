// Opt-in, bounded live qualification. Fixtures contain synthetic speech/images.
package main

import (
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

func main() {
	if err := check(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func check() error {
	home, _ := os.UserHomeDir()
	config := flag.String("config", filepath.Join(home, ".config/agent-c/gateway.json"), "Private deployed gateway config")
	target := flag.String("gateway", "", "Optional local loopback gateway origin")
	audioFile := flag.String("audio", "", "Synthetic PCM16LE mono 16kHz fixture")
	imageFile := flag.String("image", "", "Synthetic JPEG fixture")
	expected := flag.String("expect", "READY", "Expected word in the synthetic image")
	flag.Parse()
	var cfg struct {
		PublicURL     string `json:"public_url"`
		OperatorToken string `json:"operator_token"`
	}
	raw, err := os.ReadFile(*config)
	if err != nil || json.Unmarshal(raw, &cfg) != nil || cfg.OperatorToken == "" {
		return fmt.Errorf("private gateway config unavailable")
	}
	origin := cfg.PublicURL
	if *target != "" {
		u, e := url.Parse(*target)
		if e != nil || u.Scheme != "http" || net.ParseIP(u.Hostname()) == nil || !net.ParseIP(u.Hostname()).IsLoopback() || u.User != nil || u.Path != "" || u.RawQuery != "" {
			return fmt.Errorf("gateway override must be a loopback HTTP origin")
		}
		origin = *target
	}
	u, err := url.Parse(origin)
	if err != nil {
		return fmt.Errorf("gateway origin invalid")
	}
	u.Path = "/voice/v1/realtime"
	if u.Scheme == "https" {
		u.Scheme = "wss"
	} else {
		u.Scheme = "ws"
	}
	pcm, err := os.ReadFile(*audioFile)
	if err != nil || len(pcm) < 3200 || len(pcm) > 20*32000 || len(pcm)%2 != 0 {
		return fmt.Errorf("bounded synthetic PCM fixture required")
	}
	jpeg, err := os.ReadFile(*imageFile)
	if err != nil || len(jpeg) > 190<<10 {
		return fmt.Errorf("bounded synthetic JPEG fixture required")
	}
	dialer := websocket.Dialer{HandshakeTimeout: 15 * time.Second, Proxy: http.ProxyFromEnvironment}
	conn, resp, err := dialer.Dial(u.String(), http.Header{"Authorization": []string{"Bearer " + cfg.OperatorToken}, "Origin": []string{cfg.PublicURL}})
	if err != nil {
		code := 0
		if resp != nil {
			code = resp.StatusCode
			resp.Body.Close()
		}
		return fmt.Errorf("gateway voice handshake failed (status %d)", code)
	}
	defer conn.Close()
	conn.SetReadLimit(1 << 20)
	conn.SetReadDeadline(time.Now().Add(55 * time.Second))
	for {
		var event map[string]any
		if conn.ReadJSON(&event) != nil {
			return fmt.Errorf("voice setup incomplete")
		}
		if event["type"] == "error" {
			return fmt.Errorf("provider rejected setup")
		}
		if event["type"] == "session.updated" {
			break
		}
	}
	sent := make(chan error, 1)
	go func() {
		send := func(kind, key, value string) error {
			conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			return conn.WriteJSON(map[string]string{"type": kind, "event_id": fmt.Sprint(time.Now().UnixNano()), key: value})
		}
		if e := send("input_audio_buffer.append", "audio", base64.StdEncoding.EncodeToString(make([]byte, 640))); e != nil {
			sent <- e
			return
		}
		if e := send("input_image_buffer.append", "image", base64.StdEncoding.EncodeToString(jpeg)); e != nil {
			sent <- e
			return
		}
		// Real-time pacing also exercises the gateway's input rate bounds.
		pcm = append(pcm, make([]byte, 48000)...)
		for start := 0; start < len(pcm); start += 640 {
			if start > 0 && start%35200 == 0 {
				if e := send("input_image_buffer.append", "image", base64.StdEncoding.EncodeToString(jpeg)); e != nil {
					sent <- e
					return
				}
			}
			end := min(start+640, len(pcm))
			if e := send("input_audio_buffer.append", "audio", base64.StdEncoding.EncodeToString(pcm[start:end])); e != nil {
				sent <- e
				return
			}
			time.Sleep(20 * time.Millisecond)
		}
		sent <- nil
	}()
	audioBytes := 0
	transcript := ""
	asr := false
	complete := false
	inputTranscript := ""
	for {
		var event map[string]any
		if conn.ReadJSON(&event) != nil {
			return fmt.Errorf("voice roundtrip incomplete (audio=%t captions=%t asr=%t done=%t)", audioBytes > 0, transcript != "", asr, complete)
		}
		switch event["type"] {
		case "error":
			return fmt.Errorf("provider returned a voice error")
		case "conversation.item.input_audio_transcription.completed":
			asr = true
			inputTranscript, _ = event["transcript"].(string)
		case "response.audio.delta":
			if s, ok := event["delta"].(string); ok {
				b, _ := base64.StdEncoding.DecodeString(s)
				audioBytes += len(b)
			}
		case "response.audio_transcript.done":
			transcript, _ = event["transcript"].(string)
		case "response.text.done":
			if transcript == "" {
				transcript, _ = event["text"].(string)
			}
		case "response.done":
			complete = true
		}
		if complete && audioBytes > 0 && transcript != "" && asr {
			break
		}
	}
	if err := <-sent; err != nil {
		return fmt.Errorf("synthetic input delivery failed")
	}
	understood := strings.Contains(strings.ToUpper(transcript), strings.ToUpper(*expected))
	report := map[string]any{"gateway_roundtrip": true, "input_asr": asr, "output_pcm_bytes": audioBytes, "output_caption": true, "synthetic_caption": transcript, "synthetic_input_caption": inputTranscript, "image_understood": understood, "synthetic_input": true}
	b, _ := json.Marshal(report)
	fmt.Println(string(b))
	if !understood {
		return fmt.Errorf("image response did not contain the expected word")
	}
	return nil
}
