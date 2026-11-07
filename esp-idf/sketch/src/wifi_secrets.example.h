// Copy this file to wifi_secrets.h (same folder) and fill it in.
// wifi_secrets.h is in .gitignore so the password and key stay off GitHub.
#pragma once

#define WIFI_SSID        "your-network"
#define WIFI_PASS        "your-password"     // "" for an open network

// The Netlify site, no trailing slash
#define BADGE_SERVER     "https://YOUR-SITE.netlify.app"

// Must match UPLOAD_KEY in the Netlify site's environment variables
#define BADGE_UPLOAD_KEY "change-me"
