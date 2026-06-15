# Prompt Engineering — 核心技法速查

> 提炼自 [Prompt Engineering Guide](https://www.promptingguide.ai/) 和 [Awesome Prompt Engineering](https://github.com/promptslab/Awesome-Prompt-Engineering)

## 六要素提示结构

设计 prompt 时包含以下要素，结果更可靠：

1. **Instruction** — 明确指令（做什么、怎么做）
2. **Context** — 背景信息或外部知识
3. **Input Data** — 待处理的输入数据
4. **Output Indicator** — 输出格式标记（如 `Answer:` `JSON:`）
5. **Examples** — 1~5 个示例（Few-shot）
6. **Constraints** — 约束条件（长度、格式、禁止项）

## 核心技法

### Zero-Shot（零样本）
不加示例，仅靠指令。适合简单任务。
```
Classify the text into neutral, negative, or positive.
Text: {input}
Sentiment:
```

### Few-Shot（少样本）
提供 1~N 个示例，模型从示例中学习模式。示例的**格式**比标签正确性更重要。
```
English: Hello → French: Bonjour
English: Thank you → French: Merci
English: Goodbye → French:
```

### Chain-of-Thought（思维链，CoT）
要求模型分步推理。适合数学、逻辑、复杂决策。
```
Q: 15, 32, 5, 13, 82, 7, 1 中的奇数之和是否为偶数？
A: 先列出奇数: 15, 5, 13, 7, 1。求和: 15+5+13+7+1=41。41不是偶数。所以答案: False。
```

### Role Prompting（角色提示）
赋予模型一个专业角色，框定行为边界。
```
你是一位有10年经验的嵌入式C语言审查者。请审查以下代码...
```

### Structured Output（结构化输出）
明确要求输出格式：JSON、YAML、表格、列表等。
```
Return ONLY valid JSON: {"sentiment": "positive"|"negative"|"neutral", "confidence": 0.0-1.0}
```

## 通用原则

1. **具体优于模糊** — "写3个要点，每点不超过50字" 优于 "写个总结"
2. **正面指令优于负面指令** — "输出JSON" 优于 "不要输出其他格式"
3. **先给上下文，再给指令** — 模型对前置信息更敏感
4. **用分隔符隔离不同部分** — `### Context ###` `---` 等
5. **温度参数** — 事实型任务用低温（0~0.3），创意型任务用高温（0.7~1.0）
6. **测试边界情况** — 空输入、超长输入、对抗样本都要测

## 参考资源

- 完整指南: https://www.promptingguide.ai/
- Anthropic 官方指南: https://docs.anthropic.com/en/docs/build-with-claude/prompt-engineering/overview
- The Prompt Report (58+技法综述): https://arxiv.org/abs/2406.06608
- Awesome 资源列表: https://github.com/promptslab/Awesome-Prompt-Engineering
