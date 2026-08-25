package main

import (
	"bytes"
	"context"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"net/url"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
	"github.com/skip2/go-qrcode"

	setupassets "github.com/avncharlie/spotify3ds/assets"
	"github.com/avncharlie/spotify3ds/internal/setupoauth"
	"github.com/avncharlie/spotify3ds/internal/setupqr"
)

const dashboardURL = "https://developer.spotify.com/dashboard"

var (
	green       = color.NRGBA{R: 0x1d, G: 0xb9, B: 0x54, A: 0xff}
	greenBright = color.NRGBA{R: 0x22, G: 0xd8, B: 0x68, A: 0xff}
	background  = color.NRGBA{R: 0x08, G: 0x08, B: 0x08, A: 0xff}
	panel       = color.NRGBA{R: 0x12, G: 0x12, B: 0x12, A: 0xff}
	border      = color.NRGBA{R: 0x30, G: 0x30, B: 0x30, A: 0xff}
	muted       = color.NRGBA{R: 0x88, G: 0x88, B: 0x88, A: 0xff}
	white       = color.NRGBA{R: 0xf2, G: 0xf2, B: 0xf2, A: 0xff}
	errorColor  = color.NRGBA{R: 0xe5, G: 0x66, B: 0x66, A: 0xff}
)

type setupTheme struct{ fyne.Theme }

func (setupTheme) Color(name fyne.ThemeColorName, variant fyne.ThemeVariant) color.Color {
	switch name {
	case theme.ColorNameBackground:
		return background
	case theme.ColorNameForeground:
		return white
	case theme.ColorNamePrimary:
		return green
	case theme.ColorNameForegroundOnPrimary:
		return color.NRGBA{A: 0xff}
	case theme.ColorNameHyperlink:
		return greenBright
	case theme.ColorNameDisabledButton:
		return color.NRGBA{R: 0x16, G: 0x43, B: 0x29, A: 0xff}
	case theme.ColorNameButton, theme.ColorNameInputBackground:
		return panel
	case theme.ColorNameDisabled, theme.ColorNamePlaceHolder:
		return color.NRGBA{R: 0x58, G: 0x58, B: 0x58, A: 0xff}
	case theme.ColorNameSeparator:
		return border
	case theme.ColorNameHover:
		return color.NRGBA{R: 0x18, G: 0x28, B: 0x1e, A: 0xff}
	case theme.ColorNameFocus:
		return green
	}
	return theme.DefaultTheme().Color(name, variant)
}

func (setupTheme) Size(name fyne.ThemeSizeName) float32 {
	switch name {
	case theme.SizeNameText:
		return 14
	case theme.SizeNameHeadingText:
		return 23
	case theme.SizeNamePadding:
		return 8
	case theme.SizeNameInputBorder:
		return 1
	}
	return theme.DefaultTheme().Size(name)
}

type mode int

const (
	modeWalkthrough mode = iota
	modeConnecting
	modeQR
)

type wizard struct {
	app    fyne.App
	window fyne.Window
	step   int
	mode   mode

	clientEntry    *widget.Entry
	validation     *canvas.Text
	action         *primaryButton
	clientOK       bool
	clientID       string
	cancel         context.CancelFunc
	authorizeURL   string
	authorizeEntry *selectableEntry
	qrResult       *setupoauth.Result
}

func main() {
	a := app.NewWithID("com.avncharlie.spotify3ds.setup")
	a.SetIcon(fyne.NewStaticResource("spotify3ds-setup.png", setupassets.SetupIcon))
	a.Settings().SetTheme(setupTheme{Theme: theme.DefaultTheme()})
	w := &wizard{
		app: a, window: a.NewWindow("Spotify3DS Setup"),
	}
	w.window.Resize(fyne.NewSize(384, 593))
	w.window.SetFixedSize(true)
	w.window.SetMainMenu(fyne.NewMainMenu(
		fyne.NewMenu("Setup",
			fyne.NewMenuItem("Show Spotify3DS install QR", w.showInstallQR)),
	))
	w.buildPersistentControls()
	w.render()
	w.window.ShowAndRun()
}

func (w *wizard) buildPersistentControls() {
	w.clientEntry = widget.NewEntry()
	w.clientEntry.SetPlaceHolder("paste the client ID")
	w.clientEntry.OnChanged = w.clientChanged
	w.validation = text("", 12, muted, false)
	w.validation.Hide()
	w.action = newPrimaryButton("Connect Spotify", w.actionTapped)
	w.clientEntry.SetText(w.clientID)
}

func (w *wizard) render() {
	switch w.mode {
	case modeConnecting:
		w.window.SetContent(w.connectingPane())
	case modeQR:
		w.window.SetContent(w.qrPane())
	default:
		w.window.SetContent(w.walkthroughPane())
	}
}

func (w *wizard) walkthroughPane() fyne.CanvasObject {
	w.clientEntry.Enable()
	w.action.SetText("Connect Spotify")
	if w.clientOK {
		w.action.Enable()
	} else {
		w.action.Disable()
	}

	top := container.NewVBox(
		text("Getting your Spotify client ID", 23, white, true),
		container.NewGridWithColumns(3,
			newPill(0, "Create", w.step, w.setStep),
			newPill(1, "Fill in", w.step, w.setStep),
			newPill(2, "Copy ID", w.step, w.setStep)),
		fixedHeight(w.stepContent(), 321),
		w.stepButtons(),
	)
	bottom := w.credentialArea(false)
	return pageInset(container.NewVBox(top, layout.NewSpacer(), bottom))
}

func (w *wizard) stepContent() fyne.CanvasObject {
	switch w.step {
	case 0:
		dashboardLink := widget.NewHyperlink("developer.spotify.com/dashboard", mustURL(dashboardURL))
		dashboardLink.SizeName = theme.SizeNameText
		return container.NewVBox(
			instructionBlock(
				textLine(
					text("Sign in at ", 14, white, false),
					dashboardLink,
					text(" and", 14, white, false)),
				textLine(text("press Create app.", 14, white, false)),
			),
			tutorialImage("dashboard", setupassets.DashboardStep, image.Rectangle{}, fyne.NewSize(360, 188), true),
		)
	case 1:
		redirect := newSelectableEntry(setupoauth.RedirectURI)
		var copyButton *outlineButton
		copyButton = newOutlineButton("Copy", true, func() {
			w.window.Clipboard().SetContent(setupoauth.RedirectURI)
			copyButton.SetLabel("Copied")
			time.AfterFunc(1500*time.Millisecond, func() {
				fyne.Do(func() {
					if copyButton.label == "Copied" {
						copyButton.SetLabel("Copy")
					}
				})
			})
		})
		return container.NewVBox(
			instructionBlock(
				textLine(text("Name it anything. Add this as the redirect URI,", 14, white, false)),
				textLine(text("tick Web API, and save.", 14, white, false)),
			),
			container.NewBorder(nil, nil, nil, copyButton, redirect),
			tutorialImage("redirect", setupassets.CreateAppStep,
				image.Rectangle{}, fyne.NewSize(360, 220), true),
		)
	case 2:
		return container.NewVBox(
			instructionBlock(
				textLine(text("Now copy your app's Client ID and paste it below.", 14, white, false)),
			),
			tutorialImage("client-id", setupassets.ClientIDStep,
				image.Rectangle{}, fyne.NewSize(360, 175), true),
		)
	}
	return nil
}

func (w *wizard) stepButtons() fyne.CanvasObject {
	back := newOutlineButton("Back", w.step > 0, func() {
		if w.step > 0 {
			w.step--
			w.render()
		}
	})
	next := newOutlineButton("Next", w.step < 2, func() {
		if w.step < 2 {
			w.step++
			w.render()
		}
	})
	left := fyne.CanvasObject(layout.NewSpacer())
	return container.NewHBox(left, layout.NewSpacer(), back, next)
}

func (w *wizard) credentialArea(disabled bool) fyne.CanvasObject {
	if disabled {
		w.clientEntry.Disable()
	} else {
		w.clientEntry.Enable()
	}
	return container.NewVBox(
		widget.NewSeparator(),
		w.validation,
		w.clientEntry,
		w.action,
	)
}

func (w *wizard) connectingPane() fyne.CanvasObject {
	w.clientEntry.Disable()
	dot := container.NewGridWrap(fyne.NewSize(10, 10), canvas.NewCircle(green))
	status := container.NewHBox(container.NewCenter(dot), text("Waiting for Spotify", 17, white, false))
	callback := text("127.0.0.1:8888/callback", 12, muted, false)
	callback.TextStyle = fyne.TextStyle{Monospace: true}
	cardContent := container.NewVBox(
		status,
		progressTrack(0.42),
		callback,
	)
	cardBackground := canvas.NewRectangle(panel)
	cardBackground.CornerRadius = 5
	cardBackground.StrokeColor = border
	cardBackground.StrokeWidth = 1
	cardBackground.SetMinSize(fyne.NewSize(0, 84))
	card := container.NewStack(cardBackground, container.NewPadded(cardContent))
	reopen := widget.NewHyperlink("Open the login page again", nil)
	reopen.OnTapped = func() {
		if w.authorizeURL != "" {
			_ = w.app.OpenURL(mustURL(w.authorizeURL))
		}
	}
	w.authorizeEntry = newSelectableEntry(w.authorizeURL)
	var copyLink *outlineButton
	copyLink = newOutlineButton("Copy", true, func() {
		if w.authorizeURL == "" {
			return
		}
		w.window.Clipboard().SetContent(w.authorizeURL)
		copyLink.SetLabel("Copied")
		time.AfterFunc(1500*time.Millisecond, func() {
			fyne.Do(func() {
				if copyLink.label == "Copied" {
					copyLink.SetLabel("Copy")
				}
			})
		})
	})
	authorizeRow := container.NewBorder(nil, nil, nil, copyLink, w.authorizeEntry)
	back := newOutlineButton("Back", true, func() { w.cancelAuthorization() })
	next := newOutlineButton("Next", false, func() {})
	top := container.NewVBox(
		text("Approve in your browser", 23, white, true),
		instructionBlock(
			textLine(text("Spotify's login page is open in your browser. Agree there", 14, muted, false)),
			textLine(text("and come back.", 14, muted, false)),
		),
		card,
		container.NewHBox(layout.NewSpacer(), back, next),
	)
	cancel := newOutlineButton("Cancel", true, w.cancelAuthorization)
	bottom := container.NewVBox(
		reopen,
		authorizeRow,
		widget.NewSeparator(),
		w.clientEntry,
		cancel,
	)
	return pageInset(container.NewVBox(top, layout.NewSpacer(), bottom))
}

func (w *wizard) qrPane() fyne.CanvasObject {
	if w.qrResult == nil {
		return container.NewCenter(widget.NewLabel("No QR is available."))
	}
	payload, err := setupqr.Encode(w.qrResult.Credentials)
	if err != nil {
		return container.NewCenter(widget.NewLabel(err.Error()))
	}
	code, err := qrcode.New(string(payload), qrcode.Medium)
	if err != nil {
		return container.NewCenter(widget.NewLabel(err.Error()))
	}
	png, err := code.PNG(480)
	if err != nil {
		return container.NewCenter(widget.NewLabel(err.Error()))
	}
	decoded, _, err := image.Decode(bytes.NewReader(png))
	if err != nil {
		return container.NewCenter(widget.NewLabel(err.Error()))
	}
	qr := canvas.NewImageFromImage(decoded)
	qr.FillMode = canvas.ImageFillContain
	qr.ScaleMode = canvas.ImageScalePixels
	qr.SetMinSize(fyne.NewSize(260, 260))
	qrBackground := canvas.NewRectangle(white)
	qrBackground.CornerRadius = 5
	qrCard := container.NewStack(
		qrBackground,
		container.NewCenter(qr),
	)
	qrBackground.SetMinSize(fyne.NewSize(360, 267))
	back := newOutlineButton("Back", true, func() {
		w.mode = modeWalkthrough
		w.step = 2
		w.render()
	})
	done := newPrimaryButton("Done", w.window.Close)
	startBadge := keyBadge("START")
	instructions := instructionBlock(
		textLine(
			text("Open Spotify3DS, press ", 14, white, false),
			startBadge,
			text(", point the camera at the", 14, white, false)),
		textLine(text("code.", 14, white, false)),
	)
	save := widget.NewHyperlink("Save creds.cfg to the SD card instead", nil)
	save.OnTapped = w.saveCredentials
	content := container.NewVBox(
		text("Scan it on your 3DS", 23, white, true),
		qrCard,
		instructions,
		layout.NewSpacer(),
		save,
		widget.NewSeparator(),
		container.NewBorder(nil, nil, back, nil, done),
	)
	return pageInset(content)
}

func (w *wizard) saveCredentials() {
	if w.qrResult == nil {
		return
	}
	file := dialog.NewFileSave(func(writer fyne.URIWriteCloser, err error) {
		if err != nil {
			dialog.ShowError(err, w.window)
			return
		}
		if writer == nil {
			return
		}
		_, writeErr := fmt.Fprintf(writer, "client_id=%s\nrefresh_token=%s\n",
			w.qrResult.Credentials.ClientID, w.qrResult.Credentials.RefreshToken)
		closeErr := writer.Close()
		if writeErr != nil {
			dialog.ShowError(writeErr, w.window)
		} else if closeErr != nil {
			dialog.ShowError(closeErr, w.window)
		}
	}, w.window)
	file.SetFileName("creds.cfg")
	file.Show()
}

func (w *wizard) clientChanged(value string) {
	w.clientID = strings.TrimSpace(value)
	w.clientOK = false
	w.action.Disable()
	if len(w.clientID) != 32 {
		w.validation.Text = ""
		w.validation.Hide()
		w.validation.Refresh()
		return
	}
	for _, character := range w.clientID {
		if !((character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9')) {
			w.validation.Text = "Client ID contains unsupported characters"
			w.validation.Color = errorColor
			w.validation.Show()
			w.validation.Refresh()
			return
		}
	}
	w.validation.Text = ""
	w.validation.Hide()
	w.validation.Refresh()
	w.clientOK = true
	w.action.Enable()
}

func (w *wizard) actionTapped() {
	if w.mode == modeConnecting {
		w.cancelAuthorization()
		return
	}
	if !w.clientOK {
		return
	}
	w.authorizeURL = ""
	w.mode = modeConnecting
	w.render()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	w.cancel = cancel
	go func() {
		result, err := setupoauth.Authorize(ctx, w.clientID, func(raw string) error {
			fyne.Do(func() {
				w.authorizeURL = raw
				if w.authorizeEntry != nil {
					w.authorizeEntry.SetText(raw)
				}
			})
			return w.app.OpenURL(mustURL(raw))
		}, func(string) {})
		cancel()
		fyne.Do(func() {
			if err != nil {
				w.mode = modeWalkthrough
				w.validation.Text = err.Error()
				w.validation.Color = errorColor
				w.validation.Refresh()
				w.render()
				return
			}
			w.qrResult = &result
			w.mode = modeQR
			w.render()
		})
	}()
}

func (w *wizard) cancelAuthorization() {
	if w.cancel != nil {
		w.cancel()
	}
	w.mode = modeWalkthrough
	w.step = 2
	w.render()
}

func (w *wizard) setStep(step int) {
	w.step = step
	w.render()
}

func (w *wizard) showInstallQR() {
	qr := imageFromResource("cia-qr.png", setupassets.CIAQRCode, fyne.NewSize(260, 260))
	dialog.NewCustom("Install Spotify3DS", "Back to setup", container.NewVBox(
		wrapped("In FBI choose Remote Install → Scan QR Code."),
		container.NewCenter(qr),
	), w.window).Show()
}

func newPill(step int, label string, selected int, tapped func(int)) fyne.CanvasObject {
	pill := &stepPill{
		step: step, label: label, active: step == selected,
		complete: step < selected, tapped: func() { tapped(step) },
	}
	pill.ExtendBaseWidget(pill)
	return pill
}

type primaryButton struct {
	widget.BaseWidget
	label   string
	enabled bool
	tapped  func()
}

func newPrimaryButton(label string, tapped func()) *primaryButton {
	button := &primaryButton{label: label, enabled: true, tapped: tapped}
	button.ExtendBaseWidget(button)
	return button
}

func (button *primaryButton) Tapped(*fyne.PointEvent) {
	if button.enabled && button.tapped != nil {
		button.tapped()
	}
}

func (*primaryButton) TappedSecondary(*fyne.PointEvent) {}

func (button *primaryButton) Cursor() desktop.Cursor {
	if button.enabled {
		return desktop.PointerCursor
	}
	return desktop.DefaultCursor
}

func (button *primaryButton) SetText(label string) {
	button.label = label
	button.Refresh()
}

func (button *primaryButton) Enable() {
	button.enabled = true
	button.Refresh()
}

func (button *primaryButton) Disable() {
	button.enabled = false
	button.Refresh()
}

func (button *primaryButton) CreateRenderer() fyne.WidgetRenderer {
	background := canvas.NewRectangle(green)
	background.CornerRadius = 5
	label := canvas.NewText(button.label, color.NRGBA{A: 0xff})
	label.TextStyle = fyne.TextStyle{Bold: true}
	renderer := &primaryRenderer{
		button: button, background: background, label: label,
		objects: []fyne.CanvasObject{background, label},
	}
	renderer.Refresh()
	return renderer
}

type primaryRenderer struct {
	button     *primaryButton
	background *canvas.Rectangle
	label      *canvas.Text
	objects    []fyne.CanvasObject
}

func (renderer *primaryRenderer) Layout(size fyne.Size) {
	renderer.background.Resize(size)
	labelSize := renderer.label.MinSize()
	renderer.label.Resize(labelSize)
	renderer.label.Move(fyne.NewPos(
		(size.Width-labelSize.Width)/2,
		(size.Height-labelSize.Height)/2,
	))
}

func (*primaryRenderer) MinSize() fyne.Size { return fyne.NewSize(120, 42) }

func (renderer *primaryRenderer) Refresh() {
	backgroundColor := green
	textColor := color.Color(color.NRGBA{A: 0xff})
	if !renderer.button.enabled {
		backgroundColor = color.NRGBA{R: 0x16, G: 0x43, B: 0x29, A: 0xff}
		textColor = color.NRGBA{R: 0x5c, G: 0x86, B: 0x69, A: 0xff}
	}
	renderer.background.FillColor = backgroundColor
	renderer.label.Text = renderer.button.label
	renderer.label.Color = textColor
	renderer.label.TextSize = 14
	renderer.label.TextStyle = fyne.TextStyle{Bold: true}
	renderer.background.Refresh()
	renderer.label.Refresh()
	renderer.Layout(renderer.button.Size())
}

func (renderer *primaryRenderer) Objects() []fyne.CanvasObject { return renderer.objects }
func (*primaryRenderer) Destroy()                              {}

type outlineButton struct {
	widget.BaseWidget
	label   string
	enabled bool
	tapped  func()
}

func newOutlineButton(label string, enabled bool, tapped func()) *outlineButton {
	button := &outlineButton{label: label, enabled: enabled, tapped: tapped}
	button.ExtendBaseWidget(button)
	return button
}

func (button *outlineButton) Tapped(*fyne.PointEvent) {
	if button.enabled && button.tapped != nil {
		button.tapped()
	}
}

func (button *outlineButton) TappedSecondary(*fyne.PointEvent) {}

func (button *outlineButton) Cursor() desktop.Cursor {
	if button.enabled {
		return desktop.PointerCursor
	}
	return desktop.DefaultCursor
}

func (button *outlineButton) SetLabel(label string) {
	button.label = label
	button.Refresh()
}

func (button *outlineButton) CreateRenderer() fyne.WidgetRenderer {
	background := canvas.NewRectangle(color.Transparent)
	background.CornerRadius = 5
	background.StrokeWidth = 1
	label := canvas.NewText(button.label, white)
	label.Alignment = fyne.TextAlignCenter
	renderer := &outlineRenderer{button: button, background: background,
		label: label, objects: []fyne.CanvasObject{background, label}}
	renderer.Refresh()
	return renderer
}

type outlineRenderer struct {
	button     *outlineButton
	background *canvas.Rectangle
	label      *canvas.Text
	objects    []fyne.CanvasObject
}

func (renderer *outlineRenderer) Layout(size fyne.Size) {
	renderer.background.Resize(size)
	labelSize := renderer.label.MinSize()
	renderer.label.Resize(labelSize)
	renderer.label.Move(fyne.NewPos(
		(size.Width-labelSize.Width)/2,
		(size.Height-labelSize.Height)/2,
	))
}

func (renderer *outlineRenderer) MinSize() fyne.Size { return fyne.NewSize(62, 42) }

func (renderer *outlineRenderer) Refresh() {
	colour := color.Color(border)
	textColour := white
	if !renderer.button.enabled {
		colour = color.NRGBA{R: 0x22, G: 0x22, B: 0x22, A: 0xff}
		textColour = color.NRGBA{R: 0x55, G: 0x55, B: 0x55, A: 0xff}
	}
	renderer.background.StrokeColor = colour
	renderer.background.FillColor = color.Transparent
	renderer.label.Text = renderer.button.label
	renderer.label.Color = textColour
	renderer.label.TextSize = 14
	renderer.background.Refresh()
	renderer.label.Refresh()
}

func (renderer *outlineRenderer) Objects() []fyne.CanvasObject { return renderer.objects }
func (renderer *outlineRenderer) Destroy()                     {}

type stepPill struct {
	widget.BaseWidget
	step             int
	label            string
	active, complete bool
	tapped           func()
}

func (pill *stepPill) Tapped(*fyne.PointEvent) {
	if pill.tapped != nil {
		pill.tapped()
	}
}

func (pill *stepPill) TappedSecondary(*fyne.PointEvent) {}

func (*stepPill) Cursor() desktop.Cursor { return desktop.PointerCursor }

func (pill *stepPill) CreateRenderer() fyne.WidgetRenderer {
	background := canvas.NewRectangle(panel)
	background.CornerRadius = 5
	number := canvas.NewText("", muted)
	check := canvas.NewImageFromResource(fyne.NewStaticResource("step-check.svg", []byte(
		`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 12 10"><path d="M1 5 L4 8 L11 1" fill="none" stroke="#888888" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>`)))
	check.FillMode = canvas.ImageFillStretch
	check.ScaleMode = canvas.ImageScaleSmooth
	label := canvas.NewText("", muted)
	renderer := &pillRenderer{
		pill: pill, background: background, number: number,
		check: check, label: label,
		objects: []fyne.CanvasObject{background, number, check, label},
	}
	renderer.Refresh()
	return renderer
}

type pillRenderer struct {
	pill       *stepPill
	background *canvas.Rectangle
	number     *canvas.Text
	check      *canvas.Image
	label      *canvas.Text
	objects    []fyne.CanvasObject
}

func (renderer *pillRenderer) Layout(size fyne.Size) {
	renderer.background.Resize(size)
	numberSize := renderer.number.MinSize()
	labelSize := renderer.label.MinSize()
	const gap = 8
	prefixWidth := numberSize.Width
	if renderer.pill.complete {
		prefixWidth = 12
	}
	contentWidth := prefixWidth + gap + labelSize.Width
	x := (size.Width - contentWidth) / 2
	if renderer.pill.complete {
		renderer.check.Resize(fyne.NewSize(12, 10))
		renderer.check.Move(fyne.NewPos(x, (size.Height-10)/2))
	} else {
		renderer.number.Resize(numberSize)
		renderer.number.Move(fyne.NewPos(x, (size.Height-numberSize.Height)/2))
	}
	renderer.label.Resize(labelSize)
	renderer.label.Move(fyne.NewPos(x+prefixWidth+gap, (size.Height-labelSize.Height)/2))
}

func (renderer *pillRenderer) MinSize() fyne.Size { return fyne.NewSize(104, 44) }

func (renderer *pillRenderer) Refresh() {
	renderer.background.FillColor = panel
	renderer.number.Color = muted
	renderer.label.Color = muted
	if renderer.pill.complete {
		renderer.number.Hide()
		renderer.check.Show()
	} else {
		renderer.number.Show()
		renderer.check.Hide()
	}
	if renderer.pill.active {
		renderer.background.FillColor = white
		renderer.number.Color = muted
		renderer.label.Color = color.NRGBA{R: 0x12, G: 0x12, B: 0x12, A: 0xff}
	}
	renderer.number.Text = fmt.Sprintf("%d", renderer.pill.step+1)
	renderer.number.TextSize = 14
	renderer.number.TextStyle = fyne.TextStyle{}
	renderer.label.Text = renderer.pill.label
	renderer.label.TextSize = 14
	renderer.label.TextStyle = fyne.TextStyle{Bold: renderer.pill.active}
	renderer.background.Refresh()
	renderer.number.Refresh()
	renderer.check.Refresh()
	renderer.label.Refresh()
}

func (renderer *pillRenderer) Objects() []fyne.CanvasObject { return renderer.objects }
func (renderer *pillRenderer) Destroy()                     {}

func cropResource(name string, data []byte, rectangle image.Rectangle) fyne.Resource {
	decoded, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return fyne.NewStaticResource(name, data)
	}
	bounds := decoded.Bounds()
	rectangle = rectangle.Intersect(bounds)
	type subImager interface {
		SubImage(image.Rectangle) image.Image
	}
	sub, ok := decoded.(subImager)
	if !ok || rectangle.Empty() {
		return fyne.NewStaticResource(name, data)
	}
	buffer := &bytes.Buffer{}
	if err := png.Encode(buffer, sub.SubImage(rectangle)); err != nil {
		return fyne.NewStaticResource(name, data)
	}
	return fyne.NewStaticResource(name, buffer.Bytes())
}

func tutorialImage(name string, data []byte, rectangle image.Rectangle, minimum fyne.Size, preserveAspect bool) fyne.CanvasObject {
	var resource fyne.Resource = fyne.NewStaticResource(name, data)
	if !rectangle.Empty() {
		resource = cropResource(name, data, rectangle)
	}
	img := canvas.NewImageFromResource(resource)
	img.FillMode = canvas.ImageFillStretch
	if preserveAspect {
		img.FillMode = canvas.ImageFillContain
	}
	img.ScaleMode = canvas.ImageScaleSmooth
	img.SetMinSize(minimum)
	frame := canvas.NewRectangle(color.Transparent)
	frame.CornerRadius = 5
	frame.StrokeColor = border
	frame.StrokeWidth = 1
	return container.NewStack(img, frame)
}

func imageFromResource(name string, data []byte, minimum fyne.Size) *canvas.Image {
	img := canvas.NewImageFromResource(fyne.NewStaticResource(name, data))
	img.FillMode = canvas.ImageFillContain
	img.ScaleMode = canvas.ImageScalePixels
	img.SetMinSize(minimum)
	return img
}

func pageInset(content fyne.CanvasObject) fyne.CanvasObject {
	return container.New(layout.NewCustomPaddedLayout(12, 12, 12, 12), content)
}

func instructionBlock(lines ...fyne.CanvasObject) fyne.CanvasObject {
	return container.New(layout.NewCustomPaddedVBoxLayout(3), lines...)
}

func textLine(parts ...fyne.CanvasObject) fyne.CanvasObject {
	return container.New(layout.NewCustomPaddedHBoxLayout(0), parts...)
}

func fixedHeight(content fyne.CanvasObject, height float32) fyne.CanvasObject {
	return container.New(&fixedHeightLayout{height: height}, content)
}

type fixedHeightLayout struct{ height float32 }

func (fixed *fixedHeightLayout) Layout(objects []fyne.CanvasObject, size fyne.Size) {
	for _, object := range objects {
		object.Resize(size)
	}
}

func (fixed *fixedHeightLayout) MinSize([]fyne.CanvasObject) fyne.Size {
	return fyne.NewSize(0, fixed.height)
}

func keyBadge(value string) fyne.CanvasObject {
	background := canvas.NewRectangle(panel)
	background.CornerRadius = 3
	background.SetMinSize(fyne.NewSize(44, 22))
	return container.NewStack(background, container.NewCenter(text(value, 12, white, true)))
}

func progressTrack(fraction float32) fyne.CanvasObject {
	track := canvas.NewRectangle(color.NRGBA{R: 0x24, G: 0x24, B: 0x24, A: 0xff})
	track.CornerRadius = 2
	fill := canvas.NewRectangle(green)
	fill.CornerRadius = 2
	return container.New(&progressLayout{fraction: fraction}, track, fill)
}

type progressLayout struct{ fraction float32 }

func (progress *progressLayout) Layout(objects []fyne.CanvasObject, size fyne.Size) {
	objects[0].Resize(size)
	objects[1].Resize(fyne.NewSize(size.Width*progress.fraction, size.Height))
}

func (progress *progressLayout) MinSize([]fyne.CanvasObject) fyne.Size {
	return fyne.NewSize(0, 4)
}

type selectableEntry struct{ widget.Entry }

func newSelectableEntry(value string) *selectableEntry {
	entry := &selectableEntry{}
	entry.ExtendBaseWidget(entry)
	entry.TextStyle = fyne.TextStyle{Monospace: true}
	entry.SetText(value)
	return entry
}

func (*selectableEntry) TypedRune(rune) {}

func (entry *selectableEntry) TypedKey(event *fyne.KeyEvent) {
	switch event.Name {
	case fyne.KeyLeft, fyne.KeyRight, fyne.KeyHome, fyne.KeyEnd:
		entry.Entry.TypedKey(event)
	}
}

func (entry *selectableEntry) TypedShortcut(shortcut fyne.Shortcut) {
	switch shortcut.(type) {
	case *fyne.ShortcutCopy, *fyne.ShortcutSelectAll:
		entry.Entry.TypedShortcut(shortcut)
	}
}

func (*selectableEntry) TappedSecondary(*fyne.PointEvent) {}

func wrapped(value string) *widget.Label {
	label := widget.NewLabel(value)
	label.Wrapping = fyne.TextWrapWord
	return label
}

func text(value string, size float32, colour color.Color, bold bool) *canvas.Text {
	label := canvas.NewText(value, colour)
	label.TextSize = size
	label.TextStyle = fyne.TextStyle{Bold: bold}
	return label
}

func mustURL(raw string) *url.URL {
	parsed, _ := url.Parse(raw)
	return parsed
}
