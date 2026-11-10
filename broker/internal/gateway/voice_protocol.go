package gateway

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"image/jpeg"
	"io"
	"time"
)

type voiceInput struct {
	audio                                 bool
	at                                    time.Time
	audioTokens, imageTokens, eventTokens float64
}

func newVoiceInput() *voiceInput {
	return &voiceInput{at: time.Now(), audioTokens: 256 << 10, imageTokens: 2, eventTokens: 200}
}
func strictVoiceJSON(data []byte, value any) error {
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if d.Decode(value) != nil || d.Decode(new(any)) != io.EOF {
		return errors.New("invalid voice event")
	}
	return nil
}
func (v *voiceInput) validate(data []byte, now time.Time) error {
	if len(data) == 0 || len(data) > 384<<10 {
		return errors.New("voice event exceeds limit")
	}
	var event struct {
		EventID string `json:"event_id"`
		Type    string `json:"type"`
		Audio   string `json:"audio,omitempty"`
		Image   string `json:"image,omitempty"`
	}
	if strictVoiceJSON(data, &event) != nil || len(event.EventID) > 128 {
		return errors.New("invalid voice event")
	}
	elapsed := now.Sub(v.at).Seconds()
	if elapsed < 0 {
		elapsed = 0
	}
	v.at = now
	v.audioTokens = min(256<<10, v.audioTokens+elapsed*(128<<10))
	v.imageTokens = min(2, v.imageTokens+elapsed)
	v.eventTokens = min(200, v.eventTokens+elapsed*100)
	if v.eventTokens < 1 {
		return errors.New("voice event rate exceeded")
	}
	v.eventTokens--
	switch event.Type {
	case "input_audio_buffer.append":
		if event.Image != "" {
			return errors.New("unexpected image")
		}
		audio, err := base64.StdEncoding.Strict().DecodeString(event.Audio)
		if err != nil || len(audio) == 0 || len(audio) > 64<<10 || len(audio)%2 != 0 || float64(len(audio)) > v.audioTokens {
			return errors.New("invalid audio frame")
		}
		v.audioTokens -= float64(len(audio))
		v.audio = true
	case "input_image_buffer.append":
		if !v.audio || event.Audio != "" || len(event.Image) > 256<<10 || v.imageTokens < 1 {
			return errors.New("invalid image timing")
		}
		image, err := base64.StdEncoding.Strict().DecodeString(event.Image)
		if err != nil || len(image) > 190<<10 {
			return errors.New("invalid image")
		}
		size, err := jpeg.DecodeConfig(bytes.NewReader(image))
		if err != nil || size.Width <= 0 || size.Height <= 0 || max(size.Width, size.Height) > 1920 || min(size.Width, size.Height) > 1080 {
			return errors.New("invalid image format")
		}
		v.imageTokens--
	case "input_audio_buffer.commit", "input_audio_buffer.clear", "response.create", "response.cancel":
		if event.Audio != "" || event.Image != "" {
			return errors.New("unexpected media")
		}
	default:
		return errors.New("unsupported voice event")
	}
	return nil
}
