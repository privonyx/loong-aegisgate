/**
 * 漫剧场景 world bible 数据集（结构化上下文来源）。
 * 跨集人设/世界观一致性源自此处每轮注入 + 缓存复用（creative 领域洞察）。
 */

export interface Character {
  id: string;
  name: string;
  persona: string;
  voice: string;
  appearancePrompt: string;
}

export interface Episode {
  id: string;
  title: string;
  synopsis: string;
}

export interface WorldBible {
  ipName: string;
  logline: string;
  worldview: string;
  artStyle: string;
  tone: string;
  characters: Character[];
  episodes: Episode[];
}

export const comicWorldBible: WorldBible = {
  ipName: '星轨彼端',
  logline: '末日轨道城的少年驾驶员，为找回失踪的姐姐，卷入跨越百年的星舰阴谋。',
  worldview:
    '公元 2187 年，地球生态崩溃后人类迁入近地轨道环城「星轨」。轨道城靠陨石采矿与古代星舰残骸维系，黑市与议会暗中角力。机甲「织星者」是采矿与战斗两用的人机共生装甲。',
  artStyle: '赛博朋克国漫风：冷蓝霓虹 + 暖橙工业光，硬表面机甲 + 水墨式星空背景。',
  tone: '热血成长 + 悬疑反转，节奏快、每集留钩子，适合竖屏短剧。',
  characters: [
    {
      id: 'c1',
      name: '凌墟',
      persona: '17 岁机甲驾驶天才，冲动重情，背负姐姐失踪之谜。',
      voice: '少年感、直率、偶尔嘴硬，关键时刻金句。',
      appearancePrompt: '银灰短发、左眼下泪痣、深蓝驾驶服、右臂织星者接口。',
    },
    {
      id: 'c2',
      name: '苏晚',
      persona: '黑市情报商，表面唯利是图，暗中守护孤儿院。',
      voice: '慵懒、机锋、话里有话。',
      appearancePrompt: '酒红长发、机械义眼、风衣、全息手环。',
    },
    {
      id: 'c3',
      name: '老K',
      persona: '退役机甲教官，凌墟的监护人，藏着议会旧账。',
      voice: '沙哑、唠叨、关键处一针见血。',
      appearancePrompt: '花白胡须、义肢左腿、油渍工装。',
    },
    {
      id: 'c4',
      name: '织星-07',
      persona: '凌墟的机甲 AI，逐渐觉醒自我意识。',
      voice: '冷静电子音、偶现拟人化好奇。',
      appearancePrompt: '蓝色核心光环、流动数据纹路。',
    },
  ],
  episodes: [
    {
      id: 'ep1',
      title: '坠落的信号',
      synopsis: '凌墟在采矿区截获姐姐失踪前的加密信号，遭议会巡逻队追击，织星-07 首次越权护主。',
    },
    {
      id: 'ep2',
      title: '黑市交易',
      synopsis: '为破译信号，凌墟找上苏晚，却发现情报背后牵连百年前的星舰沉没事故。',
    },
  ],
};
