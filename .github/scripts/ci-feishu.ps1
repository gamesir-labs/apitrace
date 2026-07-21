# ASCII-only PowerShell helper for Feishu cards on Windows runners.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# StrictMode-safe property access for optional GitHub event JSON fields.
function Get-NotePropertyValue {
  param(
    $Object,
    [Parameter(Mandatory = $true)][string]$Name,
    $Default = $null
  )
  if ($null -eq $Object) {
    return $Default
  }
  $property = $Object.PSObject.Properties[$Name]
  if ($null -eq $property) {
    return $Default
  }
  return $property.Value
}

function Send-FeishuCard {
  param(
    [Parameter(Mandatory = $true)][string]$WebhookUrl,
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Template,
    [Parameter(Mandatory = $true)][string]$Markdown,
    [string]$Subtitle = '',
    [string]$ActionText = '',
    [string]$ActionUrl = ''
  )

  if ([string]::IsNullOrWhiteSpace($WebhookUrl)) {
    Write-Host 'FEISHU_WEBHOOK_URL is not configured; skipping notification.'
    return
  }

  $header = [ordered]@{
    title    = [ordered]@{ tag = 'plain_text'; content = $Title }
    template = $Template
  }
  if (-not [string]::IsNullOrWhiteSpace($Subtitle)) {
    $header.subtitle = [ordered]@{ tag = 'plain_text'; content = $Subtitle }
  }

  $elements = @(
    [ordered]@{ tag = 'markdown'; content = $Markdown }
  )
  if (-not [string]::IsNullOrWhiteSpace($ActionText) -and
      -not [string]::IsNullOrWhiteSpace($ActionUrl)) {
    $elements += [ordered]@{
      tag       = 'button'
      text      = [ordered]@{ tag = 'plain_text'; content = $ActionText }
      type      = 'primary'
      width     = 'default'
      size      = 'medium'
      behaviors = @(
        [ordered]@{
          type        = 'open_url'
          default_url = $ActionUrl
        }
      )
      margin    = '12px 0px 0px 0px'
    }
  }

  $payload = [ordered]@{
    msg_type = 'interactive'
    card     = [ordered]@{
      schema = '2.0'
      config = [ordered]@{ update_multi = $true }
      header = $header
      body   = [ordered]@{
        direction = 'vertical'
        padding   = '12px 12px 12px 12px'
        elements  = $elements
      }
    }
  }

  $json = $payload | ConvertTo-Json -Depth 20 -Compress
  $bytes = [Text.Encoding]::UTF8.GetBytes($json)
  Invoke-RestMethod -Method Post -Uri $WebhookUrl `
    -ContentType 'application/json; charset=utf-8' `
    -Body $bytes | Out-Null
}
