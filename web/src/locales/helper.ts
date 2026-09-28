/**
 * 通用的语言模块映射类型，表示可以嵌套的对象结构
 */
interface LanguageModule {
  [key: string]: string | LanguageModule;
}
type LanguageFileMap = Record<string, LanguageModule>;

export function getZhCnLang() {
  const langFiles = import.meta.glob<LanguageModule>("./zh-CN/**/*.json", {
    import: "default",
    eager: true,
  });
  const result = organizeLanguageFiles(langFiles);
  return result;
}

export function getEnUsLang() {
  const langFiles = import.meta.glob<LanguageModule>("./en-US/**/*.json", {
    import: "default",
    eager: true,
  });
  const result = organizeLanguageFiles(langFiles);
  return result;
}

export function organizeLanguageFiles(files: LanguageFileMap) {
  const result: LanguageModule = {};

  for (const key in files) {
    const data = files[key];
    const fileArr = key?.split("/");
    const fileName = fileArr[fileArr?.length - 1];
    if (!fileName) continue;
    const name = fileName.split(".json")[0];
    if (name) result[name] = data;
  }

  return result;
}
