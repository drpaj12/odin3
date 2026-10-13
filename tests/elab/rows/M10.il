# generated: elab-check --write
attribute \top 1
module \m10_implicit
  wire input 1 \a
  wire output 2 \y
  wire output 3 \z
  wire \w
  wire \n1
  wire \n2
  cell \m10_leaf \u1
    connect \d \a
    connect \e \n1
  end
  cell \m10_leaf \u2
    connect \d \n1
    connect \e \n2
  end
  connect \y \n1
  connect \z \n2
  connect \w \a
end
module \m10_leaf
  wire input 1 \d
  wire output 2 \e
  connect \e \d
end
