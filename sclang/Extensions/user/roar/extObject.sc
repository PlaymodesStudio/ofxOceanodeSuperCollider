+Object {
	|> {
		|other|
		^other.value(this)
	}

	+> {
		|other, adverb|
		^(this +.(adverb) other.value(this))
	}

	*> {
		|other, adverb|
		^(this *.(adverb) other.value(this))
	}

	++> {
		|other, adverb|
		^(this.asCollection ++.(adverb) other.value(this).asCollection)
	}

	||> {
		|other|
		^this.collect(_ |> other)
	}

	>|| {
		|other|
		^other.collect(this |> _)
	}

	expBlend { arg that, blendFrac = 0.5, min=1;
		var offset = max(0, min - this);
		^blendFrac.linexp(0, 1, this + offset, that + offset) - offset;
	}
}