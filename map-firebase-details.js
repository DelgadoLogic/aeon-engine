const { google } = require('googleapis');
const fs = require('fs');

async function main() {
    const token = process.argv[2];

    const domain = "browseaeon.com";
    const parent = "projects/aeon-browser-delgado/sites/aeon-browser-delgado";

    try {
        const res = await fetch(`https://firebasehosting.googleapis.com/v1beta1/${parent}/domains`, {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
                'Content-Type': 'application/json',
                'X-Goog-User-Project': 'aeon-browser-delgado'
            },
            body: JSON.stringify({
                site: "aeon-browser-delgado",
                domainName: domain
            })
        });

        const data = await res.json();
        console.log("Details for " + domain + ":", JSON.stringify(data.error?.details, null, 2));

        const res3 = await fetch(`https://firebasehosting.googleapis.com/v1beta1/${parent}/domains`, {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
                'Content-Type': 'application/json',
                'X-Goog-User-Project': 'aeon-browser-delgado'
            },
            body: JSON.stringify({
                site: "aeon-browser-delgado",
                domainName: "aeonbrowse.com"
            })
        });

        const data3 = await res3.json();
        console.log("Details for aeonbrowse.com:", JSON.stringify(data3.error?.details, null, 2));

    } catch (err) {
        console.error("Failed to map Firebase", err);
    }
}

main();
