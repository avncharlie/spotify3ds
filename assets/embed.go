package setupassets

import _ "embed"

//go:embed icon-setup-1024.png
var SetupIcon []byte

//go:embed setup-dev-dashboard-tutorial/1-go-to-dashboard.png
var DashboardStep []byte

//go:embed setup-dev-dashboard-tutorial/2-create-app.png
var CreateAppStep []byte

//go:embed setup-dev-dashboard-tutorial/3-get-client-ID.png
var ClientIDStep []byte
