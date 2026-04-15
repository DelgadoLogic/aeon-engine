const { google } = require('googleapis');

async function main() {
    const auth = new google.auth.GoogleAuth({
        keyFile: './sa.json',
        scopes: ['https://www.googleapis.com/auth/cloud-platform'],
    });

    const client = await auth.getClient();
    const token = await client.getAccessToken();

    const domain = "browseaeon.com";
    const parent = "projects/aeon-browser-delgado/sites/aeon-browser-delgado";

    try {
        const res = await fetch(`https://firebasehosting.googleapis.com/v1beta1/${parent}/domains`, {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token.token}`,
                'Content-Type': 'application/json'
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
                'Authorization': `Bearer ${token.token}`,
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                site: "aeon-browser-delgado",
                domainName: "www." + domain
            })
        });

        const data2 = await res2.json();
        console.log("Firebase Mapping Result WWW:", data2);
    } catch (err) {
        console.error("Failed to map Firebase", err);
    }
}

main();
