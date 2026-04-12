<#
.SYNOPSIS
    Extracts essential root CA certificates from Mozilla's cacert.pem 
    and generates BearSSL-compatible trust_anchors.h

.DESCRIPTION
    Parses PEM certificates using .NET X509Certificate2, extracts the 
    DER-encoded DN and public key components, and outputs them as C 
    arrays suitable for BearSSL's br_x509_trust_anchor structures.

    Only extracts the minimal set of root CAs needed for >90% web coverage:
    - ISRG Root X1 (Let's Encrypt) 
    - ISRG Root X2 (Let's Encrypt ECDSA)
    - DigiCert Global Root G2
    - Google Trust Services GTS Root R1
    - GlobalSign Root CA R3
#>

param(
    [string]$InputPem = "$env:TEMP\cacert.pem",
    [string]$OutputHeader = ".\trust_anchors.h"
)

# CAs we want to extract (CN substrings)
$targetCAs = @(
    "ISRG Root X1",
    "ISRG Root X2",
    "DigiCert Global Root G2",
    "GTS Root R1",
    "GlobalSign Root CA - R3"
)

# Parse all certificates from PEM
$pemContent = Get-Content $InputPem -Raw
$certMatches = [regex]::Matches($pemContent, '(?ms)-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----')

Write-Host "Total certificates in bundle: $($certMatches.Count)"

$selectedCerts = @()

foreach ($match in $certMatches) {
    $b64 = $match.Groups[1].Value -replace '\s+', ''
    $der = [Convert]::FromBase64String($b64)
    
    try {
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList @(,$der)
        $cn = $cert.Subject
        
        foreach ($target in $targetCAs) {
            if ($cn -like "*$target*") {
                Write-Host "  FOUND: $cn"
                $selectedCerts += @{
                    Name = $target -replace '[^a-zA-Z0-9]', '_'
                    Cert = $cert
                    DER = $der
                    Subject = $cn
                }
                break
            }
        }
    } catch {
        # Skip malformed certs
    }
}

Write-Host "`nSelected $($selectedCerts.Count) of $($targetCAs.Count) target CAs"

if ($selectedCerts.Count -eq 0) {
    Write-Error "No target CAs found! Check CA names."
    exit 1
}

# Helper: format byte array as C hex literal
function Format-CArray {
    param([byte[]]$Data, [string]$Indent = "    ")
    $lines = @()
    for ($i = 0; $i -lt $Data.Length; $i += 12) {
        $chunk = $Data[$i..([Math]::Min($i + 11, $Data.Length - 1))]
        $hex = ($chunk | ForEach-Object { "0x{0:X2}" -f $_ }) -join ", "
        $lines += "$Indent$hex,"
    }
    return $lines -join "`n"
}

# Build the header
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("/* trust_anchors.h — Auto-generated BearSSL trust anchors")
[void]$sb.AppendLine(" * DelgadoLogic | Security Engineer")
[void]$sb.AppendLine(" *")
[void]$sb.AppendLine(" * Generated from Mozilla CA bundle (cacert.pem)")
[void]$sb.AppendLine(" * Date: $(Get-Date -Format 'yyyy-MM-dd')")
[void]$sb.AppendLine(" *")
[void]$sb.AppendLine(" * Contains $($selectedCerts.Count) root CAs covering >90% of HTTPS traffic:")
foreach ($ca in $selectedCerts) {
    [void]$sb.AppendLine(" *   - $($ca.Subject)")
}
[void]$sb.AppendLine(" *")
[void]$sb.AppendLine(" * To regenerate: powershell -File gen_trust_anchors.ps1")
[void]$sb.AppendLine(" * LICENSE: Certificate data is public. This header is proprietary.")
[void]$sb.AppendLine(" */")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#ifndef TRUST_ANCHORS_H__")
[void]$sb.AppendLine("#define TRUST_ANCHORS_H__")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#include ""bearssl/inc/bearssl.h""")
[void]$sb.AppendLine("")

$taIndex = 0
foreach ($ca in $selectedCerts) {
    $cert = $ca.Cert
    $name = $ca.Name
    
    # Get raw DER-encoded subject DN
    $rawDN = $cert.SubjectName.RawData
    
    # Get public key info
    $pubKey = $cert.PublicKey
    $keyAlg = $pubKey.Oid.FriendlyName
    
    [void]$sb.AppendLine("/* ---- $($ca.Subject) ---- */")
    [void]$sb.AppendLine("")
    
    # DN bytes
    [void]$sb.AppendLine("static const unsigned char TA_DN${taIndex}[] = {")
    [void]$sb.AppendLine((Format-CArray -Data $rawDN))
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
    
    if ($keyAlg -eq "RSA") {
        $rsaKey = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($cert)
        $params = $rsaKey.ExportParameters($false)
        $modulus = $params.Modulus
        $exponent = $params.Exponent
        
        [void]$sb.AppendLine("static const unsigned char TA_RSA_N${taIndex}[] = {")
        [void]$sb.AppendLine((Format-CArray -Data $modulus))
        [void]$sb.AppendLine("};")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("static const unsigned char TA_RSA_E${taIndex}[] = {")
        [void]$sb.AppendLine((Format-CArray -Data $exponent))
        [void]$sb.AppendLine("};")
        [void]$sb.AppendLine("")
    } elseif ($keyAlg -eq "ECC") {
        # EC public key - extract raw point
        $ecKey = [System.Security.Cryptography.X509Certificates.ECDsaCertificateExtensions]::GetECDsaPublicKey($cert)
        $ecParams = $ecKey.ExportParameters($false)
        # Uncompressed point: 0x04 || X || Y
        $point = @([byte]0x04) + $ecParams.Q.X + $ecParams.Q.Y
        
        # Determine curve ID
        $curveName = $ecParams.Curve.Oid.FriendlyName
        $curveId = switch -Wildcard ($curveName) {
            "*256*" { "BR_EC_secp256r1" }
            "*384*" { "BR_EC_secp384r1" }
            "*521*" { "BR_EC_secp521r1" }
            default { "BR_EC_secp256r1" }
        }
        
        [void]$sb.AppendLine("static const unsigned char TA_EC_Q${taIndex}[] = {")
        [void]$sb.AppendLine((Format-CArray -Data ([byte[]]$point)))
        [void]$sb.AppendLine("};")
        [void]$sb.AppendLine("")
    }
    
    $taIndex++
}

# Build the TAs array
[void]$sb.AppendLine("/* ---- Trust Anchor Array ---- */")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const br_x509_trust_anchor TAs[] = {")

$taIndex = 0
foreach ($ca in $selectedCerts) {
    $cert = $ca.Cert
    $keyAlg = $cert.PublicKey.Oid.FriendlyName
    
    [void]$sb.AppendLine("    {")
    [void]$sb.AppendLine("        { (unsigned char *)TA_DN${taIndex}, sizeof TA_DN${taIndex} },")
    [void]$sb.AppendLine("        BR_X509_TA_CA,")
    
    if ($keyAlg -eq "RSA") {
        [void]$sb.AppendLine("        {")
        [void]$sb.AppendLine("            BR_KEYTYPE_RSA,")
        [void]$sb.AppendLine("            { .rsa = {")
        [void]$sb.AppendLine("                (unsigned char *)TA_RSA_N${taIndex}, sizeof TA_RSA_N${taIndex},")
        [void]$sb.AppendLine("                (unsigned char *)TA_RSA_E${taIndex}, sizeof TA_RSA_E${taIndex}")
        [void]$sb.AppendLine("            } }")
        [void]$sb.AppendLine("        }")
    } else {
        $ecKey = [System.Security.Cryptography.X509Certificates.ECDsaCertificateExtensions]::GetECDsaPublicKey($cert)
        $ecParams = $ecKey.ExportParameters($false)
        $curveName = $ecParams.Curve.Oid.FriendlyName
        $curveId = switch -Wildcard ($curveName) {
            "*256*" { "BR_EC_secp256r1" }
            "*384*" { "BR_EC_secp384r1" }
            "*521*" { "BR_EC_secp521r1" }
            default { "BR_EC_secp256r1" }
        }
        
        [void]$sb.AppendLine("        {")
        [void]$sb.AppendLine("            BR_KEYTYPE_EC,")
        [void]$sb.AppendLine("            { .ec = {")
        [void]$sb.AppendLine("                ${curveId},")
        [void]$sb.AppendLine("                (unsigned char *)TA_EC_Q${taIndex}, sizeof TA_EC_Q${taIndex}")
        [void]$sb.AppendLine("            } }")
        [void]$sb.AppendLine("        }")
    }
    
    [void]$sb.AppendLine("    },")
    $taIndex++
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#define TAs_NUM  $($selectedCerts.Count)")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#endif /* TRUST_ANCHORS_H__ */")

# Write output
$sb.ToString() | Set-Content $OutputHeader -Encoding UTF8
Write-Host "`nGenerated: $OutputHeader ($($selectedCerts.Count) trust anchors)"
