# Lights the tree's 4 strings in distinct dim colors (1 red, 2 green, 3 blue, 4 white) over
# the WiFi command protocol - checks string/pin order and makes output glitches easy to see.
# See docs/LED_OUTPUT.md.
param([string]$TreeHost = "neotree.local")
$client = New-Object System.Net.Sockets.TcpClient
$client.NoDelay = $true
$client.Connect($TreeHost, 7777)
$stream = $client.GetStream()
$stream.ReadTimeout = 3000

function Read-Exactly([int]$n) {
    $buf = New-Object byte[] $n; $have = 0
    while ($have -lt $n) { $got = $stream.Read($buf, $have, $n - $have); if ($got -le 0) { throw "closed" }; $have += $got }
    return ,$buf
}
function Read-Frame { $h = Read-Exactly 2; return ,(Read-Exactly ($h[0] + 256 * $h[1])) }
function Send-Msg([byte[]]$msg) {
    for ($try = 0; $try -lt 20; $try++) {
        $frame = [byte[]](@([byte]($msg.Length -band 0xFF), [byte]($msg.Length -shr 8)) + $msg)
        $stream.Write($frame, 0, $frame.Length)
        $ack = Read-Frame
        if ($ack[2] -eq 0) { return }
        if ($ack[2] -ne 1) { throw "rejected: type $($ack[1]) status $($ack[2])" }
        Start-Sleep -Milliseconds (5 * ($try + 1))   # QUEUE_FULL: back off
    }
    throw "queue stayed full"
}

$hello = Read-Frame
if ($hello[0] -ne 0x81) { throw "not the tree" }
Send-Msg ([byte[]](3, 0, 0, 0))   # all off

$strings = @(
    @{ first = 0;   last = 299; rgb = @(40, 0, 0);   name = "string 1 (GP2)   LEDs 0-299   RED" },
    @{ first = 300; last = 599; rgb = @(0, 40, 0);   name = "string 2 (GP5)   LEDs 300-599 GREEN" },
    @{ first = 600; last = 799; rgb = @(0, 0, 40);   name = "string 3 (GP6)   LEDs 600-799 BLUE" },
    @{ first = 800; last = 999; rgb = @(25, 25, 25); name = "string 4 (GP7)   LEDs 800-999 WHITE" }
)
$sent = 0
foreach ($s in $strings) {
    for ($i = $s.first; $i -le $s.last; $i += 12) {
        $n = [Math]::Min(12, $s.last - $i + 1)
        $msg = New-Object System.Collections.Generic.List[byte]
        $msg.Add(2); $msg.Add([byte]$n)
        for ($k = 0; $k -lt $n; $k++) {
            $idx = $i + $k
            $msg.Add([byte]($idx -band 0xFF)); $msg.Add([byte]($idx -shr 8))
            $msg.Add([byte]$s.rgb[0]); $msg.Add([byte]$s.rgb[1]); $msg.Add([byte]$s.rgb[2])
        }
        Send-Msg $msg.ToArray(); $sent++
    }
    Write-Output $s.name
}
Write-Output "done - $sent group messages, all ACKed"
$client.Close()
