/**
 * 电商场景数据集（商品目录）。用于验证通用骨架：换业务=换 dataset+tools。
 */

export interface Product {
  id: string;
  name: string;
  category: string;
  price: number;
  stock: number;
  description: string;
}

export interface ProductCatalog {
  storeName: string;
  products: Product[];
}

export const ecommerceCatalog: ProductCatalog = {
  storeName: '星河数码',
  products: [
    { id: 'p1', name: '静界 Pro 主动降噪耳机', category: '音频', price: 899, stock: 120, description: '40dB 主动降噪，40 小时续航，多点连接。' },
    { id: 'p2', name: '轨迹 87 客制化机械键盘', category: '外设', price: 599, stock: 60, description: 'Gasket 结构，热插拔轴，三模连接。' },
    { id: 'p3', name: '脉冲 GT 智能手表', category: '穿戴', price: 1299, stock: 35, description: '血氧/心率/睡眠监测，14 天续航，独立 eSIM。' },
    { id: 'p4', name: '闪能 165W 氮化镓充电宝', category: '配件', price: 329, stock: 200, description: '20000mAh，165W 输出，可上飞机。' },
    { id: 'p5', name: '澄空 27 4K 专业显示器', category: '显示', price: 2499, stock: 18, description: '27 寸 4K，99% AdobeRGB，Type-C 90W 反向供电。' },
  ],
};
