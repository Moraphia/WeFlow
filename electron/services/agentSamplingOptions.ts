export type AgentSamplingConfig = {
  protocol?: string
  model?: string
}

const OPENAI_RESPONSES_REASONING_MODEL = /^(?:gpt-(?:5|6)|o[1-4](?:-|$))/

export function omitsTemperature(config: AgentSamplingConfig): boolean {
  if (config.protocol === 'anthropic') return true
  const modelId = String(config.model || '').trim().toLocaleLowerCase()
  return config.protocol === 'openai-responses' && OPENAI_RESPONSES_REASONING_MODEL.test(modelId)
}
