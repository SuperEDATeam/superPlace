# 初始布局相关

> 🔗 原文链接：[初始布局相关](https://my.feishu.cn/docx/QuIRdHKPJorvVyxoG3KcrNJpnNd)
>

---

# 一、模型修正

## 背景与思想

在芯片布局阶段，我们的目标是让所有模块在芯片平面上分布合理，同时连接线 nets 尽可能短。如果一个网络 net 连接了多个引脚（pins），我们可以用这些引脚的坐标来计算它的“线长”。最常见的度量方式叫做**半周长线长（Half-Perimeter Wirelength, HPWL）**：

$$
HPWL=(x_{max}-x_{min})+(y_{max}-y_{min})
$$

它表示了这个网络的**最小包围矩形的周长一半**，也就是连线在平面上可能占据的最小距离范围。在真实芯片中，HPWL 与布线实际长度高度相关，因此被广泛用作优化目标。

如果能直接精确计算每根连线的总长度，那当然最好，但问题在于：

- 连接线数量往往成千上万，每条线连接多个引脚（pins）
- pin 之间的距离关系是**非线性且离散的**，难以直接求解最优解

于是工程师们想到一个办法：

- 把线的“拉扯关系”用弹簧来模拟。模块之间如果相连，就像被弹簧拉着，线越长，弹簧的势能越大。

这时整个芯片系统的能量就可以写成一个**二次型函数**：

$$
E=\sum_{i,j} w_{ij}(x_i-x_j)^2
$$

其中：

- $x_i,\ x_j$ 表示两个针脚的位置；
- $w_{ij}$ 是它们连接的强度（权重，weight）；
- 最小化 $E$ 就是在让系统“静止”——也就是线长尽可能短。

这样一来，原本复杂的非线性线长问题就被近似成一个连续、可求导的**二次优化问题**。它的好处是：

- 能转化为线性方程 $Ax=b$，用数值方法求解；
- 每个模块的“受力”都能线性表达；
- 为后续的全局放置（mGP / cGP）提供一个平滑、稳定的初始解。

## 公式的推导

阅读 Craftwerk-II 论文，该论文提出了 bound2bound 模型的概念，并将模型提炼成了二次公式的形式。

而文中给出的权重公式如下：

$$
w_{x,pq}^{B2B}=\begin{cases} 0, & if\ p\ and\ q\ are\ inner\ pins\\ \frac{2}{P-1}\frac{1}{|x_p^{pin}-x_q^{pin}|} & else. \end{cases}
$$

这里或许有人会疑惑，明明是二次函数，和后续要求解的线性代数又有什么关系呢？别急，我们慢慢来。

### 从单个连接开始

我们先从最简单的情况出发：假设只有两个针脚 $x_1,\ x_2$，它们之间的“连线能量”由二次项定义：

$$
E=w(x_1-x_2)^2
$$

- $w$ 为连接强度（weight），表示“这条线拉得多紧”

将括号展开：

$$
E=w(x_1^2-2x_1x_2+x_2^2)
$$

我们可以把它写成矩阵形式。设位置向量为：

$$
x=\begin{bmatrix} x_1\\ x_2 \end{bmatrix}
$$

那么 $E$ 可以重写为：

$$
E=x^T \begin{bmatrix} w & -w \\ -w & w \end{bmatrix} x
$$

- 这个矩阵正好体现了“自己对自己 $+w$，对邻居 $-w$”的关系。
- 这也是线性代数里典型的**拉普拉斯矩阵（Laplacian Matrix）**形式。

### 引入偏移量和所属节点坐标

需要注意的是，上述的 $x$ 代表的是**针脚（pin）**的**绝对坐标（absolute position）**，实际布局过程中，由于作用的单位最终为**节点（node）**，因此针脚所带来的权重，最终要回归到其所属节点本身。

将节点的坐标当作未知数 $x$，针脚相对于节点的偏移量命名为 $\mathit{offset}$：

$$
E=w((x_1+\mathit{offset}_1)-(x_2+\mathit{offset}_2))^2
$$

展开：

$$
\begin{aligned}
E&=wx_1^2-2wx_1x_2+wx_2^2+2w(\mathit{offset}_1-\mathit{offset}_2)x_1+2w(\mathit{offset}_2-\mathit{offset}_1)x_2+\mathrm{const} \\
&=wx^T\begin{bmatrix} 1 & -1\\ -1 & 1 \end{bmatrix} x+2w(\mathit{offset}_1-\mathit{offset}_2)x^T \begin{bmatrix} 1\\-1 \end{bmatrix} +\mathrm{const} \\
&=x^TAx+x^Tb+\mathrm{const}
\end{aligned}
$$

其中：

- $A$ 是对称矩阵
- $b$ 是线性项
- $\mathrm{const}$ 是常数，对导数没有影响。

### 从单条线扩展到整个网络

如果一个模块的针脚与多个模块的针脚通过网络相连，那么每个连线都会对它贡献一部分“力”。

根据上个公式的化简结果，我们得到一个总线性方程组：

$$
E=x^TAx+x^Tb+\mathrm{const}
$$

其中：

$$
x^T=\begin{bmatrix} x_1 & x_2 & x_3 & \cdots & x_n \end{bmatrix}
$$

- $x_i$ 表示编号为 $i$ 的节点的坐标
- $n$ 为节点个数
- $A$ 是长宽均为 $n$ 的**正定矩阵**

### 引入导数

上述推导完毕之后，我们对函数进行求导求极值：

1. 先看第一个项：

$$
E_1=x^TAx
$$

展开后是：

$$
E_1=\sum_i^n \sum_j^n A_{ij} x_i x_j
$$

我们对 $x_k$ 求导：

$$
\frac{\partial E_1}{\partial x_k}=\sum_j^n(A_{kj} x_j + A_{jk} x_j)
$$

因为 $A$ 是对称矩阵（即 $A_{kj} = A_{jk}$），所以：

$$
\frac{\partial E_1}{\partial x_k} = 2 \sum_j^n A_{kj} x_j
$$

写成矩阵形式就是：

$$
\nabla_x (x^TAx) = 2Ax
$$

2. 再看线性项：

$$
E_2=x^Tb=\sum_i^n b_i x_i
$$

对 $x$ 求导：

$$
\nabla_x (x^T b)=b
$$

3. 合并结果：

$$
\nabla_x E(x) = 2Ax + b
$$

4. 最小化条件（能量平衡）

能量最小时，梯度为 0：

$$
2Ax+b=0
$$

即：

$$
Ax=-\frac{1}{2}b
$$

### 引入固定模块

回顾我们之前得到的公式：

$$
E=wx^T\begin{bmatrix} 1 & -1\\ -1 & 1 \end{bmatrix} x+2w(\mathit{offset}_1-\mathit{offset}_2)x^T \begin{bmatrix} 1\\-1 \end{bmatrix} +\mathrm{const}
$$

拓展到任意节点 $p,\ q\ (x_i,\ x_j)$：

$$
E=wx^T\begin{bmatrix} 1 & -1\\ -1 & 1 \end{bmatrix} x+2w(\mathit{offset}_i-\mathit{offset}_j)x^T \begin{bmatrix} 1\\-1 \end{bmatrix} +\mathrm{const}
$$

$$
x=\begin{bmatrix} x_i\\x_j \end{bmatrix}
$$

1. 当节点 $p$ 为宏节点时，设 $x_i$ 为 $\mathrm{const}_i$：

$$
\begin{aligned}
E&=wx_j^2-2w(\mathrm{const}_i+\mathit{offset}_i-\mathit{offset}_j)x_j+\mathrm{const} \\
&=w\begin{bmatrix} x_j \end{bmatrix} \begin{bmatrix} 1 \end{bmatrix} \begin{bmatrix} x_j \end{bmatrix} -2w(\mathit{absolute}_i-\mathit{offset}_j)\begin{bmatrix} x_j \end{bmatrix} +\mathrm{const}
\end{aligned}
$$

其中，$\mathit{absolute}_i$ 为节点 $p$ 对应的当前针脚的绝对坐标。

2. 同理，当节点 $q$ 为宏节点时，设 $x_j$ 为 $\mathrm{const}_j$：

$$
\begin{aligned}
E&=wx_i^2-2w(\mathrm{const}_j+\mathit{offset}_j-\mathit{offset}_i)x_i+\mathrm{const} \\
&=w\begin{bmatrix} x_i \end{bmatrix} \begin{bmatrix} 1 \end{bmatrix} \begin{bmatrix} x_i \end{bmatrix} -2w(\mathit{absolute}_j-\mathit{offset}_i)\begin{bmatrix} x_i \end{bmatrix} +\mathrm{const}
\end{aligned}
$$

其中，$\mathit{absolute}_j$ 为节点 $q$ 对应的当前针脚的绝对坐标。

## 求解器代入

有了刚才推导出的方程，将不同情况的线性方程带入 $Ax=-\frac{1}{2}b$ 中，令 $b=-\frac{1}{2}b$，可得：

1. $p,\ q$ 均不为宏节点：

$$
A\ +\!=\ \begin{bmatrix} w & -w\\ -w & w \end{bmatrix}
$$

$$
b\ +\!=\ \begin{bmatrix} -w(\mathit{offset}_i-\mathit{offset}_j)\\ -w(\mathit{offset}_j-\mathit{offset}_i) \end{bmatrix}
$$

2. $p$ 为宏节点，$q$ 为标准节点：

$$
A\ +\!=\ \begin{bmatrix} 0 & 0\\ 0 & w \end{bmatrix}
$$

$$
b\ +\!=\ \begin{bmatrix} 0\\ w(\mathit{absolute}_i-\mathit{offset}_j) \end{bmatrix}
$$

3. $p$ 为标准节点，$q$ 为宏节点：

$$
A\ +\!=\ \begin{bmatrix} w & 0\\ 0 & 0 \end{bmatrix}
$$

$$
b\ +\!=\ \begin{bmatrix} w(\mathit{absolute}_j-\mathit{offset}_i)\\ 0 \end{bmatrix}
$$

# 二、实现细节

注意，有些实现细节是系统所必须的，否则轻则造成计算时间浪费，重则造成求解失败。

接下来，我们按照从外到内的顺序，对一些技术上的实现细节展开论述：

## 权重公式证明

很多人会好奇，craftwerk2A 论文中的公式，即：

$$
w_{x,pq}^{B2B}=\begin{cases} 0, & if\ p\ and\ q\ are\ inner\ pins\\ \frac{1}{P-1}\frac{1}{|x_p^{pin}-x_q^{pin}|} & else. \end{cases}
$$

究竟是怎么一回事，我们不妨展开讲讲。

（这里用的分子是 1，因为 craftwerk 论文中，作者在拟合线长的时候，使用的是 1/2 的前缀【见论文公式（3）】，因此我们将这两项结合，分子变为 1）

### 待求目标

假设现在我们总共有 m 根针脚，证明目标即为：

$$
HPWL=\sum \text{weight}\cdot(x_p-x_q)^2\quad(for\ p,\ q\ in\ target\ net)
$$

将 weight 公式展开，即证：

$$
HPWL=\sum\begin{cases} 0 & if\ p\ and\ q\ are\ inner\ pins\\ \dfrac{|x_p-x_q|}{P-1} & otherwise \end{cases}
$$

### 拟合过程

知道了目标以后，我们便对刚刚的待证公式进行化简：

根据公式：

- 当 p 和 q 均为内部针脚时，线长的添加量为 0
- 相反，当 p 和 q 有一方为边界节点时，进行计算

假设我们有一个 net，其中包含 5 个针脚 p1，p2，p3，p4，p5，不妨设 p1 为 x_max、p5 为 x_min（即 x 方向的两个边界针脚）：

- 按 B2B 模型，只有涉及边界针脚的 pin pair 才有权重，添加进去的 pin pair 为：
  - p1 - p2, p1 - p3, p1 - p4, p1 - p5
  - p5 - p2, p5 - p3, p5 - p4
  （$p1p5 == p5p1$，不重复添加）
- 共 7 对；扩展到 net 的维度为 P（即该 net 中有 P 个 pin 相互连接）时，pin pair 数为 2P - 3：每个非边界针脚各与两个边界针脚相连（2(P-2) 对），再加上两个边界针脚之间的 1 对。

> **标注（整理者注）**：原文此处写"最后实际上得到了 4 对 max - min 位置组合，因此组合对数为 P - 1"，与前文枚举出的 7 对（即 2P - 3）自相矛盾，叙述有误。正确的拟合关系并非"对数相等"，而是各项伸缩相消（telescoping）：每对贡献 $\frac{1}{P-1}|x_p-x_q|$，全部 2P-3 项求和恰好等于 $\frac{(P-1)(x_{max}-x_{min})}{P-1} = x_{max}-x_{min}$，即权重分母 P-1 被精确约掉，二次型之和严格等于 x 方向的 HPWL。

### 得出结论

将刚才得到的结果带入，我们得到了：

$$
HPWL=\frac{P-1}{P-1}(x_{max}-x_{min})
$$

等式的结果显而易见，将 x 方向和 y 方向加起来，便完美拟合了线长，因此公式是正确的。

## 稀疏矩阵存储

在上一节中，我们已经完成了整个能量模型的数学推导，得到了核心的二次形式：

$$
E = x^{T} A x + b^{T} x + \mathrm{const}
$$

而要最小化能量，我们最终必须求解如下线性方程组：

$$
A x = -\frac{1}{2} b
$$

到这里，很多第一次实现 QP 的人往往会忽略最关键的一点：

**A 是一个规模巨大的矩阵，但它是稀疏的。如何存储 A，直接决定了求解器能不能跑得起来。**

这一节，我们就来讲清楚：

1. 为什么要用稀疏矩阵？
2. Eigen（我们要引入的 BiCGSTAB 求解库）中关于稀疏矩阵的数据结构是怎样工作的？

而这也将引入一个新概念——**CSR 编码（Compressed Sparse Row）**。

### 为什么要用稀疏矩阵

先来观察一下 A 的结构：

根据上一节的推导，每条 pin-pair 贡献四个值：

- `A[i][i] += w`
- `A[i][j] -= w`
- `A[j][i] -= w`
- `A[j][j] += w`

这是一个典型的拉普拉斯矩阵结构。

**特点 1：只在相邻节点之间存在非零项**

一个标准单元一般会：

- 针脚属于若干个 net（3 - 6）
- 每个 net 又会连接到另外几个 pin（3 - 10）

因此一个 node 产生连接的其他 node 的数量大致为：

$$
degree(i) = 9-60
$$

因此，一行的非零项数量为：

$$
1\ （代表对角\ A[i][i]）+ degree(i)
$$

这里我们采取保守估计：

**一行最多 30-100 个非零元素。**

而矩阵的大小一般为：

$$
Density=N^2
$$

其中，$N$ 表示矩阵的行数，即 node 节点个数。

非零项数量为：

$$
30N-100N\ （每行的非零项\times行数）
$$

非零项比例：

$$
\frac{30N}{N^2}-\frac{100N}{N^2}=\frac{30}{N} - \frac{100}{N}
$$

假设 $N = 100000$：

$$
\frac{30}{N} - \frac{100}{N} = 0.03\% - 0.1\%
$$

即：**99.9% 以上都是 0**。

**特点 2：如果用密集矩阵（Dense）来存储的话**

假设节点数量 N = 200000，Dense 需要存储：

$$
200000^2 = 4\times10^{10}\ double
$$

一个 double = 8 byte，因此需要 **320GB**。

**特点 3：如果用稀疏矩阵（Sparse）存储，只需要存储所有的非零项**

$nnz$（number of non-zero elements，即矩阵中非零项数量）$=100N=2\times10^7\ double$，即最多 **160MB**。

因此，大部分人求解器求解的速度慢，实际上是因为没有使用稀疏矩阵进行存储。

### CSR 编码

稀疏矩阵如果用普通二维数组存，也没意义，仍然会存大量 0。

因此，工业界几乎统一使用 **CSR（Compressed Sparse Row）格式**来存储稀疏矩阵。

CSR 用三个一维数组表示矩阵所有非零元素：

1. **values[]：存非零元素的值**
2. **colIndex[]：每个值属于哪一列**
3. **rowPtr[]：每一行非零元素的起始位置（前缀和）**

**CSR 允许我们在线性时间内遍历一整行的所有非零项，而不会碰到一个 0。**

时间复杂度就是：

$$
O(nnz)
$$

对 200k 节点，这个复杂度是完全可以接受的。

而如果是简单的高斯消元，甚至是矩阵相乘：

- 假设两个矩阵 A 和 b 相乘：
- 其中一方为 N * N，另一方为 N
- 对于 A 中的 $N^2$ 个元素，每个元素都需要和 b 中的一个元素相乘
- 时间复杂度就是：

$$
O(N^2)
$$

关于 CSR 编码的搜索方式这里就不过多赘述了，有兴趣的可以自己了解，这里接着往下走。

到此为止，我们不仅得到了能量模型 A 和 b，也理解了为什么 A 必须用稀疏矩阵存储，以及为什么 CSR 是工业界的标准选择。

有了这一套“数据的存储结构”，我们才能真正进入下一步：

**构建 SparseMatrix、填入所有 pin-pair 权重，并最终调用求解器求解 Ax=b。**

## 求解器常用 API 实操（以 Eigen 为核心）

上一节我们已经通过理论推导得到了放置问题的核心方程：

$$
A x = -\frac{1}{2} b
$$

而刚刚也解释了为什么必须使用 CSR 格式来存储数据。

本节我们真正进入“实现层面”：**如何在程序中用 Eigen 构建 A、b、x，并最终求解？**

这一节会非常重要：很多同学的卡点不是数学推导，而是“不知道这些方程应该放到哪里、怎么放”。

我们按照最自然的顺序讲：

- A：如何用 SparseMatrix 构建
- x、b：如何用 VectorXf 表示
- solve：如何用迭代器求解
- Triplet：如何高效构建稀疏矩阵

一步一步走，看到最后你就会对整个流程非常清晰。

### 构建稀疏矩阵 A：用 SparseMatrix 存储矩阵结构

**为什么用 SparseMatrix？**

从上一节知道：

- A 是一个 N×N 的矩阵
- 但每一行只有有限个非零元素
- 非零项来自「相邻节点」之间
- 99.9% 的位置永远是 0

Dense 矩阵并不友好，因此 A 必须用：

```C++
Eigen::SparseMatrix<float, Eigen::RowMajor>
```

RowMajor 是关键，因为：

- Ax 运算是按行访问
- RowMajor 即为 CSR 格式，对后续求解非常友好

于是我们通常这样声明 A：

```C++
// 构建 x, y 两个方向的方程
Eigen::SparseMatrix<float, Eigen::RowMajor> X_A(nodeCount, nodeCount);
Eigen::SparseMatrix<float, Eigen::RowMajor> Y_A(nodeCount, nodeCount);
```

X_A 对应 x 方向的方程，Y_A 对应 y 方向的方程。

采用两个稀疏矩阵分别求解的方式，方便后续分别展开。

**A 的插值方式**

Eigen 通过三元组 Triplet 构造稀疏矩阵：

```C++
// 构建 Triplet 指令集
vector<Eigen::Triplet<float>> tripletListX;

// 对指令集进行添加，这里仅供举例，在实际操作中要注意分情况
tripletListX.push_back(Eigen::Triplet<float>(i, i, w));
tripletListX.push_back(Eigen::Triplet<float>(j, j, w));
tripletListX.push_back(Eigen::Triplet<float>(i, j, -w));
tripletListX.push_back(Eigen::Triplet<float>(j, i, -w));

...

// 通过 setFromTriplets 的 api 一次性输入指令，建立矩阵
X_A.setFromTriplets(tripletListX.begin(), tripletListX.end());
```

其中：

- Triplet(i,j,v) = “A[i][j] = v” 的一个数据点
- 而最终通过 setFromTriplets() 一次性建立矩阵
- 构建稀疏矩阵最重要的是“先累加信息，再统一压缩”，直接 insert 会非常慢。

### 构建向量 x 与 b：用 VectorXf 存储未知变量和线性项

在放置问题中，x 与 b 是两个长度为 nodeCount 的向量：

- **x**：未知变量（模块位置）
- **b**：线性项（来自 offset 差）

我们用 Eigen 的 VectorXf 来存储它们：

```C++
// VectorXf 代表动态长度向量（X），内部存储数据类型为 float（f）
Eigen::VectorXf X_x(nodeCount), Y_x(nodeCount);
Eigen::VectorXf X_b(nodeCount), Y_b(nodeCount);
```

**x：求解未知量（初始猜测非常重要）**

我们将所有节点的初值均设为 core region（布局区域）的**中心点**。

而在布局过程中，x 的初值来自模块当前坐标：

```C++
X_x(i) = curNode->getCenter().x;
```

这里有人会疑问，为什么要存储 x 的初值，有什么用处？

因为 **BiCGSTAB（Bi-Conjugate Gradient Stabilized）双共轭梯度稳定化方法**解方程的方法不是“解矩阵”，而是这样：

```C++
给一个初始解 x0（可以是任意值）
x1 = x0 + Δ1
x2 = x1 + Δ2
...
直到 Ax ≈ b
```

这个 x0 就叫：

- initial guess
- 初值
- 初始解
- 初始近似

这是迭代求解器的数学基础。

**因此，初始 guess 决定了求解的速度甚至能否求解。**

- 迭代求解器对初始 guess 非常敏感。
- 放置过程中，上一次布局的位置就是最好的初值。

而对放置问题而言，Ax=b 的解具有强烈的“连续性”。

在一次迭代更新之后，下次优化的最优解通常只与前一次有轻微差别，因为 net 权重、offset 只发生小幅变化。

因此，上一轮布局的位置天然就是下一轮求解器最好的初始值，而中心点正是这个初始值最好的开端。

**b：由偏移量 offset 差构成**

来自推导：

$$
b_i=-w(\mathit{offset}_i-\mathit{offset}_j)
$$

实现中就是对 X_b[i]、X_b[j] 不断累加。

强调一点：

固定节点的 b 会包含 absolute 坐标，普通节点则只包含相对偏移。（指路：第一章模型修正中求解器代入部分的公式推导）

### 使用求解器求解 Ax=b：BiCGSTAB 的流程

当 A、x、b 准备好后，我们用 Eigen 的迭代求解器来求解：

$$
Ax=b
$$

放置器使用的是：

```C++
for (int i = 0;; i++) {
    // 构建 A，b，x
    createSparseMatrix(X_A, Y_A, X_x, Y_x, X_b, Y_b);

    // 调用 BiCGSTAB 工具，构建求解器
    BiCGSTAB<Eigen::SparseMatrix<float, Eigen::RowMajor>, IdentityPreconditioner> solver;

    // 设置最大迭代次数
    solver.setMaxIterations(100);

    // 准备求解
    solver.compute(X_A);

    // 求解
    X_x = solver.solveWithGuess(X_b, X_x);
    xError = solver.error();

    // y 方向同样操作
    solver.compute(Y_A);
    Y_x = solver.solveWithGuess(Y_b, Y_x);
    yError = solver.error();

    // 写入并更新位置
    updateModuleLocation(X_x, Y_x);
    HPWL = db->calcNetBoundPins();

    // 截止条件：残差达到特定范围，或者达到最大迭代次数
    if (fabs(xError) < target_error && fabs(yError) < target_error && i > 4) {
        break;
    }
    if (i >= maxIterationNumber) {
        break;
    }
}
```

**compute(A)：准备求解**

```C++
solver.compute(X_A);
```

说明：

- 分析 A 的矩阵结构并进行预处理，以便为后续迭代做初始化

**solveWithGuess(b, x0)：以“上一轮位置”为初值求解**

```C++
X_x = solver.solveWithGuess(X_b, X_x);
```

这一句是求解过程的精髓之一：

- 我们用当前的位置解作为下一步求解的初值，这样更接近真实解，迭代也会更快

使用 `solveWithGuess(b, x_old)`，可以显著降低 BiCGSTAB 的迭代次数，使求解器在数十步内收敛，而不用从全 0 解开始慢慢“爬”。

这是一种典型的渐进式优化策略，也是放置器能在百万级节点下保持可计算性的关键之一。

**solver.error()：查看收敛情况**

```C++
xError = solver.error();
```

- error 越小说明解越稳定
- 放置器不需要极高精度，通常 1e-4 ~ 1e-5 就够用了

### 最小距离差 MIN_DISTANCE

这里顺带提一句，在计算 $weight = 1 / |x_1 - x_2|$ 时，我们不能让分母无限逼近 0，如果 pin 离得特别近，那么：

- $|x_1 - x_2|$ 接近 0
- weight = 1 / distance 会突然变成几千、几万
- 这将导致 A 矩阵变刚，BiCGSTAB 迭代直接开始震荡、不收敛

而这种情况在工程里被称为**“权重奇异点”**，因此我们可以加一句：

```C++
distanceX = max(distanceX, MIN_DISTANCE);

// 详细代码格式，仅供参考
float distanceX = fabs(pin1->getAbsolutePos().x - pin2->getAbsolutePos().x);

if (float_greaterorequal(distanceX, MIN_DISTANCE)) {
    weightX = constant1 / distanceX;
} else {
    weightX = constant1 / MIN_DISTANCE;
}
```

以使得迭代条件稳定，也能够正常收敛。

### 同一个单元内的针脚不可相互拉扯

如果在遍历针脚的时候，发现针脚 p 和 q 对应的单元相同，我们需要跳过，因为：

**同一个 module 内部的两个 pins 无论距离是多少，都不应该产生“拉扯力”，否则会错误地把模块内部当成两块要互相拉扯的东西，从而干扰布局，甚至让 A 矩阵变奇异。**

**由于同 module 的 pins 永远属于同一个变量 $x_k$**，它们之间不存在独立自由度，每对 pin 都只会形成**伪约束**，无意义且干扰求解。

因此工业界统一规定：

> **同 module 的 pin 对要跳过。**

最终整个求解过程如下：

1. 遍历 nets，构建所有 pin-pair 的四个 Laplacian 元素
2. 利用 Triplet 构造稀疏矩阵 A（CSR 格式），将偏移差累加到 b 向量中
3. 将当前布局作为 x 的初始 guess
4. 调用 BiCGSTAB 求解 Ax=b
5. 将新解写回模块位置

> **Triplet 负责构建 A，VectorXf 负责存储 b、x，BiCGSTAB 负责求解。**
