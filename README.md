# Hellokitty's Blog

基于 [Hugo](https://gohugo.io/) + [PaperMod](https://github.com/adityatelange/hugo-PaperMod) 主题搭建，通过 GitHub Actions 自动构建并部署到 GitHub Pages。

- 线上地址：https://yangyang233333.github.io/
- 主题以 git submodule 形式引入（见 `.gitmodules`）

## 目录结构

```
content/posts/   文章 Markdown 源文件（每篇一个 .md）
hugo.yaml        站点配置（标题、菜单、参数等）
themes/          PaperMod 主题（submodule）
.github/workflows/hugo.yaml   自动部署流水线
public/          Hugo 构建产物（已在 .gitignore 忽略，由 Actions 生成）
```

## 如何发布一篇新文章

### 1. 新建文章

在仓库根目录执行：

```bash
hugo new posts/my-new-post.md
```

或直接在 `content/posts/` 下手动创建一个 `.md` 文件。文件名即 URL slug，建议用英文小写 + 连字符（例如 `hash-collision-birthday-bound.md`），中文标题写在 frontmatter 里即可。

### 2. 填写 frontmatter

文件开头的 YAML 元数据：

```yaml
---
title: "文章标题"
date: 2026-08-11T15:10:00+08:00
draft: false
tags: ["标签1", "标签2"]
categories: ["分类"]
---
```

要点：

- `draft: false` 才会被发布（`true` 是草稿，构建时跳过）。
- `date` 必须 **早于或等于当前时间**。站点配置了 `buildFuture: false`，日期在未来的文章会被当作“未来文章”跳过、不生成页面。这是最容易踩的坑，写完发现文章没出现，先检查这里。

### 3. 本地预览（可选）

```bash
# 实时预览（含草稿），浏览器打开 http://localhost:1313
hugo server -D

# 或只构建、验证无报错、确认页面已生成
hugo --gc --minify
ls public/posts/<你的文章 slug>/
```

### 4. 提交并推送

```bash
git add content/posts/my-new-post.md
git commit -m "post: 文章标题"
git push origin main
```

推送到 `main` 分支后，GitHub Actions（`Deploy Hugo site to Pages`）会自动构建并部署，约 30~60 秒完成。

### 5. 确认部署状态（可选）

```bash
gh run list --limit 1        # 查看最近一次部署
gh run watch <run-id> --exit-status   # 实时跟踪直到完成
```

部署成功后访问 `https://yangyang233333.github.io/posts/<你的文章 slug>/`。GitHub Pages 边缘缓存可能有 1~2 分钟延迟。

## 如何删除一篇文章

```bash
git rm content/posts/old-post.md
git commit -m "remove: 删除某文章"
git push origin main
```

Actions 会重新构建，对应页面随之下线。

## 常见问题

- **文章 push 了但线上看不到**：
  1. 检查 `draft` 是否为 `false`；
  2. 检查 `date` 是否早于当前时间（`buildFuture: false` 会跳过未来文章）；
  3. 用 `hugo --gc --minify` 本地构建，看 `public/posts/` 下是否生成了对应目录。
- **`.hugo_build.lock` 报错**：删掉它再构建 `rm -f .hugo_build.lock`。
- **主题相关文件缺失**：确认 submodule 已拉取 `git submodule update --init --recursive`。

## 本地功能

- 深色 / 浅色模式自动切换
- 文章标签与归档
- 全文搜索
- 代码块一键复制
- 阅读时长估算
