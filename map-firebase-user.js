const { google } = require('googleapis');
const fs = require('fs');

async function main() {
    // The user token from powershell will be passed as the first argument
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
        console.log("Firebase Mapping Result:", data);
        
        const res2 = await fetch(`https://firebasehosting.googleapis.com/v1beta1/${parent}/domains`, {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
                'Content-Type': 'application/json',
                'X-Goog-User-Project': 'aeon-browser-delgado'
            },
            body: JSON.stringify({
                site: "aeon-browser-delgado",
                domainName: "www." + domain
            })
        });

        const data2 = await res2.json();
        console.log("Firebase Mapping Result WWW:", data2);

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
        console.log("Firebase Mapping Result 3:", data3);

        const res4 = await fetch(`https://firebasehosting.googleapis.com/v1beta1/${parent}/domains`, {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
                'Content-Type': 'application/json',
                'X-Goog-User-Project': 'aeon-browser-delgado'
            },
            body: JSON.stringify({
                site: "aeon-browser-delgado",
                domainName: "www.aeonbrowse.com"
            })
        });

        const data4 = await res4.json();
        console.log("Firebase Mapping Result 4:", data4);

    } catch (err) {
        console.error("Failed to map Firebase", err);
    }
}

main();
